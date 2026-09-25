/* $OpenBSD$ */

/*
 * Kitty graphics, bridged to the browser terminal's image store.
 *
 * A program that draws a picture the kitty way under a multiplexer transmits
 * it once with an APC ("\033_Ga=T,U=1,f=100,i=N,c=C,r=R;<base64>\033\\",
 * chunked with m=1 ... m=0) and then prints Unicode placeholders: U+10EEEE
 * cells whose foreground colour is the image id N and whose combining marks
 * give the tile's row and column. Claude Code does exactly this for an Image
 * element, with N a 256-colour palette index.
 *
 * tmux cannot forward the APC: only clients looking at the pane at that
 * moment would get it, and the picture is gone for every other client, for a
 * window switched to later, and after any browser reload. The placeholders
 * themselves are ordinary text and survive all of that. So instead of
 * forwarding, the transmission is written to a spool the __ccimg sidecar
 * serves (~/tmp/ccimg/<socket>/<id>_<cols>x<rows>.png), under a global id
 * that tmux assigns, and every placeholder cell this pane writes has its
 * colour rewritten from the program's local id to that global id, as RGB.
 * The browser's image addon already reads an RGB placeholder colour as a
 * 24-bit id and fetches it from the sidecar, so the picture is drawn from the
 * grid like any other cell: redraw-safe, for every client.
 *
 * Global ids have the top bit set, so they can never collide with the 1..255
 * palette ids the MessageDisplay embeds use.
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <netinet/in.h>

#include <errno.h>
#include <resolv.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tmux.h"

#define KITTY_ID_BIT 0x800000U
#define KITTY_MAX_DATA (32U * 1024 * 1024)

struct kitty_pane {
	u_int	 ids[256];	/* program's palette id -> global id */

	char	*data;		/* base64 of the transmission in progress */
	size_t	 len;
	size_t	 space;
	u_int	 image;
	u_int	 cols;
	u_int	 rows;
	int	 png;
	int	 active;
};

static const u_char kitty_placeholder_utf8[] = { 0xf4, 0x8e, 0xbb, 0xae };

/* Is this cell a Unicode placeholder? */
int
kitty_placeholder(const struct utf8_data *ud)
{
	return (ud->size == sizeof kitty_placeholder_utf8 &&
	    memcmp(ud->data, kitty_placeholder_utf8, ud->size) == 0);
}

/* The global id a pane's placeholder colour stands for, or 0. */
u_int
kitty_map(struct window_pane *wp, int fg)
{
	if (wp == NULL || wp->kitty == NULL || (~fg & COLOUR_FLAG_256))
		return (0);
	return (wp->kitty->ids[fg & 0xff]);
}

void
kitty_free(struct window_pane *wp)
{
	if (wp->kitty == NULL)
		return;
	free(wp->kitty->data);
	free(wp->kitty);
	wp->kitty = NULL;
}

static u_int
kitty_next_id(void)
{
	static u_int	next;

	/* Start somewhere different on each server so a restarted server does not
	 * overwrite spool files that scrollback on another server still names. */
	if (next == 0)
		next = (u_int)time(NULL) * 2654435761U;
	next++;
	return (KITTY_ID_BIT | (next & (KITTY_ID_BIT - 1)));
}

static int
kitty_spool(u_int id, u_int cols, u_int rows, const u_char *png, size_t len)
{
	const char	*home = getenv("HOME"), *sock;
	char		 dir[PATH_MAX], path[PATH_MAX], tmp[PATH_MAX];
	FILE		*f;

	if (home == NULL || *home == '\0')
		return (-1);
	sock = strrchr(socket_path, '/');
	sock = (sock == NULL) ? socket_path : sock + 1;

	snprintf(dir, sizeof dir, "%s/tmp/ccimg", home);
	if (mkdir(dir, 0700) != 0 && errno != EEXIST)
		return (-1);
	snprintf(dir, sizeof dir, "%s/tmp/ccimg/%s", home, sock);
	if (mkdir(dir, 0700) != 0 && errno != EEXIST)
		return (-1);

	snprintf(path, sizeof path, "%s/%u_%ux%u.png", dir, id, cols, rows);
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	if ((f = fopen(tmp, "wb")) == NULL)
		return (-1);
	if (fwrite(png, 1, len, f) != len) {
		fclose(f);
		unlink(tmp);
		return (-1);
	}
	if (fclose(f) != 0 || rename(tmp, path) != 0) {
		unlink(tmp);
		return (-1);
	}
	return (0);
}

static void
kitty_finish(struct window_pane *wp, struct kitty_pane *kp)
{
	u_char	*png;
	int	 n;
	u_int	 id;

	kp->active = 0;
	if (!kp->png || kp->image == 0 || kp->image > 255 || kp->len == 0) {
		kp->len = 0;
		return;
	}

	png = xmalloc(kp->len);
	kp->data[kp->len] = '\0';
	n = b64_pton(kp->data, png, kp->len);
	kp->len = 0;
	if (n <= 0) {
		free(png);
		return;
	}

	id = kitty_next_id();
	if (kitty_spool(id, kp->cols, kp->rows, png, n) == 0) {
		kp->ids[kp->image] = id;
		log_debug("%s: %%%u image %u -> %u (%d bytes, %ux%u)", __func__,
		    wp->id, kp->image, id, n, kp->cols, kp->rows);
	}
	free(png);
}

static void
kitty_append(struct kitty_pane *kp, const char *s, size_t n)
{
	if (kp->len + n + 1 > KITTY_MAX_DATA) {
		kp->active = 0;
		kp->len = 0;
		return;
	}
	if (kp->len + n + 1 > kp->space) {
		kp->space = (kp->len + n + 1) * 2;
		kp->data = xrealloc(kp->data, kp->space);
	}
	memcpy(kp->data + kp->len, s, n);
	kp->len += n;
}

/*
 * Handle an APC. Returns 0 when it is not a kitty graphics command (the caller
 * carries on with its own meaning of APC), 1 when it was consumed.
 */
int
kitty_apc(struct window_pane *wp, const char *buf)
{
	struct kitty_pane	*kp;
	const char		*payload, *p, *end;
	char			 action = 't', medium = 'd';
	u_int			 image = 0, cols = 0, rows = 0, format = 32;
	int			 more = 0, has_action = 0, has_image = 0;

	if (buf[0] != 'G')
		return (0);
	if (wp->kitty == NULL)
		wp->kitty = xcalloc(1, sizeof *wp->kitty);
	kp = wp->kitty;

	payload = strchr(buf + 1, ';');
	end = (payload == NULL) ? buf + strlen(buf) : payload;
	for (p = buf + 1; p < end; ) {
		const char	*comma = memchr(p, ',', end - p);
		const char	*stop = (comma == NULL) ? end : comma;
		char		 key;
		const char	*value;

		if (stop - p >= 3 && p[1] == '=') {
			key = p[0];
			value = p + 2;
			switch (key) {
			case 'a':
				action = *value;
				has_action = 1;
				break;
			case 't':
				medium = *value;
				break;
			case 'i':
				image = strtoul(value, NULL, 10);
				has_image = 1;
				break;
			case 'c':
				cols = strtoul(value, NULL, 10);
				break;
			case 'r':
				rows = strtoul(value, NULL, 10);
				break;
			case 'f':
				format = strtoul(value, NULL, 10);
				break;
			case 'm':
				more = (*value == '1');
				break;
			}
		}
		p = (comma == NULL) ? end : comma + 1;
	}

	if (has_action || has_image) {
		if (action != 't' && action != 'T')
			return (1);	/* delete, query, placement: nothing to bridge */
		kp->active = 1;
		kp->len = 0;
		kp->image = image;
		kp->cols = cols;
		kp->rows = rows;
		kp->png = (format == 100 && medium == 'd');
	} else if (!kp->active)
		return (1);	/* a stray continuation */

	if (payload != NULL)
		kitty_append(kp, payload + 1, strlen(payload + 1));
	if (!more && kp->active)
		kitty_finish(wp, kp);
	return (1);
}
