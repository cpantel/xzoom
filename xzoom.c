/* Copyright Itai Nahshon 1995, 1996.
   This program is distributed with no warranty.

   Source files for this program may be distributed freely.
   Modifications to this file are okay as long as:
    a. This copyright notice and comment are preserved and
	   left at the top of the file.
	b. The man page is fixed to reflect the change.
	c. The author of this change adds his name and change
	   description to the list of changes below.
   Executable files may be distributed with sources, or with
   exact location where the source code can be obtained.

Changelist:
Author                    Description
------                    -----------
Itai Nahshon              Version 0.1, Nov. 21 1995
Itai Nahshon              Version 0.2, Apr. 17 1996
                          include <sys/types.h>
                          Use memmove() instead of memcopy()
                          Optional macro to replace call to usleep().
Markus F.X.J. Oberhumer   Version 0.4, Feb. 18 1998
                          split into 2 files (scale.h)
                          added support for 15, 16, 24 and 32 bpp displays
                          added a grid (press key 'g')
                          optimized scaling routines
                          use memcpy() instead of memmove() ;-)
                          some other minor changes/fixes
tony mancill		2002/02/13 <tmancill@debian.org>
			hacked in support for WM_DELETE_WINDOW

Carlos Pantelides       * 2020/04/01
                          added follow mouse taken from
                          https://github.com/mbarakatt/xzoom-follow-mouse



*/
#include <assert.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/signal.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

#ifdef XSHM
#include <sys/ipc.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>
#endif

#include <X11/cursorfont.h>
#include <X11/keysym.h>

#ifdef TIMER
#include <sys/time.h>
#include <unistd.h>
#endif

Display *dpy;
Screen *scr;
Window win;
Atom wm_delete_window;
Atom wm_protocols;
Status status;

GC gc;
#ifdef FRAME
GC framegc;
#endif

#ifdef TIMER
Font font;
struct timeval old_time;
#endif

Cursor when_button;
Cursor crosshair;

char *progname;
int set_title;

#define SRC		0				/* index for source image */
#define	DST		1				/* index for dest image */

#define WIDTH	256				/* default width */
#define HEIGHT	256				/* default height */

#define MAG		2				/* default magnification */
#define MAGX	MAG				/* horizontal magnification */
#define MAGY	MAG				/* vertical magnification */

int xgrab, ygrab;				/* where do we take the picture from */

int magx = MAGX;
int magy = MAGY;

int flipxy = False;				/* flip x and y */
int flipx = False;				/* flip display about y axis */
int flipy = False;				/* flip display about x axiz */

int xzoom_flag = False;			/* next mag change only to magx */
int yzoom_flag = False;			/* next mag change only to magy */

int gridx = False;
int gridy = False;

int width[2] = { 0, WIDTH };
int height[2] = { 0, HEIGHT };
unsigned depth = 0;

#ifdef XSHM
XShmSegmentInfo shminfo[2];			/* Segment info.  */
#endif
XImage *ximage[2];					/* Ximage struct. */

int created_images = False;

#define NDELAYS 5

int delays[NDELAYS] = { 200000, 100000, 50000, 10000, 0 };
int delay_index = 0;
int delay = 200000;			/* 0.2 second between updates */

void
timeout_func(int signum) {
	set_title = True;
	signum = signum;          /* UNUSED */
}

#ifdef FRAME
#define DRAW_FRAME() \
	XDrawRectangle(dpy, RootWindowOfScreen(scr), framegc, xgrab, ygrab, width[SRC]-1, height[SRC]-1)
#endif

void
allocate_images(void) {
	int i;

	for(i = 0; i < 2; i++) {

#ifdef XSHM
		ximage[i] = XShmCreateImage(dpy,
			DefaultVisualOfScreen(scr),
			DefaultDepthOfScreen(scr),
			ZPixmap, NULL, &shminfo[i],
			width[i], height[i]);

		if(ximage[i] == NULL) {
			perror("XShmCreateImage");
			exit(-1);
		}

		shminfo[i].shmid = shmget(IPC_PRIVATE,
			(unsigned int)(ximage[i]->bytes_per_line * ximage[i]->height),
			IPC_CREAT | 0777);

		if(shminfo[i].shmid < 0) {
			perror("shmget");
			exit(-1);
		}

		shminfo[i].shmaddr = (char *)shmat(shminfo[i].shmid, 0, 0);

		if (shminfo[i].shmaddr == ((char *) -1)) {
			perror("shmat");
			exit(-1);
		}

#ifdef DEBUG
		fprintf(stderr, "new shared memory segment at 0x%08x size %d\n",
			shminfo[i].shmaddr, ximage[i]->bytes_per_line * ximage[i]->height);
#endif

		ximage[i]->data = shminfo[i].shmaddr;
		shminfo[i].readOnly = False;

		XShmAttach(dpy, &shminfo[i]);
		XSync(dpy, False);

		shmctl(shminfo[i].shmid, IPC_RMID, 0);
#else
		char *data;
		data = malloc(BitmapUnit(dpy) / 8 * width[i] * height[i]);

		ximage[i] = XCreateImage(dpy,
			DefaultVisualOfScreen(scr),
			DefaultDepthOfScreen(scr),
			ZPixmap, 0, data,
			width[i], height[i], 32, 0);

		if(ximage[i] == NULL) {
			perror("XCreateImage");
			exit(-1);
		}

#endif /* XSHM */
	}
	created_images = True;
}

void
destroy_images(void) {
	int i;

	if (!created_images)
		return;

	for(i = 0; i < 2; i++) {
#ifdef XSHM
		XShmDetach(dpy, &shminfo[i]);	/* ask X11 to detach shared segment */
		shmdt(shminfo[i].shmaddr);		/* detach it ourselves */
#else
		free(ximage[i]->data);
#endif
		ximage[i]->data = NULL;			/* remove refrence to that address */
		XDestroyImage(ximage[i]);		/* and destroy image */
	}

	created_images = False;
}

void
Usage(void) {
	fprintf(stderr, "Usage: %s [ args ]\n"
		"Command line args:\n"
		"-display displayname\n"
		"-mag magnification [ magnification ]\n"
		"-geometry geometry\n"
		"-source geometry\n"
		"-x\n"
		"-y\n"
		"-xy\n\n"
		"Window commands:\n"
		"+: Zoom in\n"
		"-: Zoom out\n"
		"x: Flip right and left\n"
		"y: Flip top and bottom\n"
		"z: Rotate 90 degrees counter-clockwize\n"
		"w: Next '+' or '-' only change width scaling\n"
		"h: Next '+' or '-' only change height scaling\n"
		"d: Change delay between frames\n"
		"q: Quit\n"
		"Arrow keys: Scroll in direction of arrow\n"
		"Mouse button drag: Set top-left corner of viewed area\n",
		progname);
	exit(1);
}

/* resize is called with the dest size.
   we call it then manification changes or when
   actual window size is changed */
void
resize(int new_width, int new_height) {

	destroy_images();		/* we can get rid of these */

	/* find new dimensions for source */

	if(flipxy) {
		height[SRC] = (new_width+magx-1) / magx;
		width[SRC] = (new_height+magy-1) / magy;
	}
	else {
		width[SRC] = (new_width+magx-1) / magx;
		height[SRC] = (new_height+magy-1) / magy;
	}

	if(width[SRC] < 1)
		width[SRC] = 1;
	if(width[SRC] > WidthOfScreen(scr))
		width[SRC] = WidthOfScreen(scr);

	if(height[SRC] < 1)
		height[SRC] = 1;
	if(height[SRC] > HeightOfScreen(scr))
		height[SRC] = HeightOfScreen(scr);

	/* temporary, the dest image may be larger than the
	   actual window */
	if(flipxy) {
		width[DST] = magx * height[SRC];
		height[DST] = magy * width[SRC];
	}
	else {
		width[DST] = magx * width[SRC];
		height[DST] = magy * height[SRC];
	}

	allocate_images();		/* allocate new images */

	/* remember actual window size */
	if(width[DST] > new_width)
		width[DST] = new_width;
	if(height[DST] > new_height)
		height[DST] = new_height;
}


void scale8(void)
{
#define T unsigned char
#include "scale.h"
#undef T
}


void scale16(void)
{
#define T unsigned short
#include "scale.h"
#undef T
}


void scale32(void)
{
#define T unsigned int
#include "scale.h"
#undef T
}

static int _XlibErrorHandler(Display *display, XErrorEvent *event) {
	fprintf(stderr, "An error occured detecting the mouse position\n");
	return True;
}

int
main(int argc, char **argv) {
	int follow_mouse = False;
	int number_of_screens;
	int i;
	Bool result;
	Window *root_windows;
	Window window_returned;
	int root_x, root_y;
	int win_x, win_y;
	unsigned int mask_return;

	Display *display = XOpenDisplay(NULL);
	assert(display);
	XSetErrorHandler(_XlibErrorHandler);
	number_of_screens = XScreenCount(display);
	fprintf(stderr, "There are %d screens available in this X session\n", number_of_screens);
	root_windows = malloc(sizeof(Window) * number_of_screens);
	for (i = 0; i < number_of_screens; i++) {
		root_windows[i] = XRootWindow(display, i);
	}
	for (i = 0; i < number_of_screens; i++) {
		result = XQueryPointer(display, root_windows[i], &window_returned,
				&window_returned, &root_x, &root_y, &win_x, &win_y,
				&mask_return);
		if (result == True) {
			break;
		}
	}
	if (result != True) {
		fprintf(stderr, "No mouse found.\n");
		return -1;
	}
	printf("Mouse is at (%d,%d)\n", root_x, root_y);

	XSetWindowAttributes xswa;
	XEvent event;

	int buttonpressed = False;
	int unmapped = True;
	int scroll = 1;
	char title[80];
	XGCValues gcv;
	char *dpyname = NULL;
	int source_geom_mask = NoValue,
		dest_geom_mask = NoValue,
		copy_from_src_mask;
	int xpos = 0, ypos = 0;

	atexit(destroy_images);
	progname = strrchr(argv[0], '/');
	if(progname)
		++progname;
	else
		progname = argv[0];

	/* parse command line options */
	while(--argc > 0) {
		++argv;

		if(!strcmp(argv[0], "-follow")) {
			follow_mouse = True;
			printf("follow mouse ON\n");
			continue;
		}

		if(argv[0][0] == '=') {
			dest_geom_mask = XParseGeometry(argv[0],
				&xpos, &ypos,
				&width[DST], &height[DST]);
			continue;
		}

		if(!strcmp(argv[0], "-mag")) {
			++argv; --argc;

			magx = argc > 0 ? atoi(argv[0]) : -1;

			if(magx <= 0)
				Usage();


			magy = argc > 1 ? atoi(argv[1]) : -1;

			if(magy <= 0)
				magy = magx;
			else {
				++argv; --argc;
			}

			continue;
		}

		if(!strcmp(argv[0], "-x")) {
			flipx = True;
			continue;
		}

		if(!strcmp(argv[0], "-y")) {
			flipy = True;
			continue;
		}

		if(!strcmp(argv[0], "-z") ||
		   !strcmp(argv[0], "-xy")) {
			flipxy = True;
			continue;
		}

		if(!strcmp(argv[0], "-source")) {
			++argv; --argc;

			if(argc < 1)
				Usage();

			source_geom_mask = XParseGeometry(argv[0],
				&xgrab, &ygrab,
				&width[SRC], &height[SRC]);

			continue;
		}

		if(!strcmp(argv[0], "-dest") ||
		   !strcmp(argv[0], "-geometry")) {
			++argv; --argc;

			if(argc < 1)
				Usage();

			dest_geom_mask = XParseGeometry(argv[0],
				&xpos, &ypos,
				&width[DST], &height[DST]);

			continue;
		}

		if(!strcmp(argv[0], "-d") ||
		   !strcmp(argv[0], "-display")) {

		   	++argv; --argc;

			if(argc < 1)
				Usage();

		   	dpyname = argv[0];
			continue;
		}

		if(!strcmp(argv[0], "-delay")) {

		   	++argv; --argc;

			if(argc < 1)
				Usage();

			if(sscanf(argv[0], "%u", &delay) != 1)
				Usage();

			delay *= 1000;

			continue;
		}

		Usage();
	}

	if (!(dpy = XOpenDisplay(dpyname))) {
		perror("Cannot open display");
		exit(-1);
	}

	/* Now, see if we have to calculate width[DST] and height[DST]
	   from the SRC parameters */

	copy_from_src_mask = NoValue;

	if(source_geom_mask & WidthValue) {
		if(flipxy) {
			height[DST] = magy * width[SRC];
			copy_from_src_mask |= HeightValue;

		}
		else {
			width[DST] = magx * width[SRC];
			copy_from_src_mask |= WidthValue;
		}
	}

	if(source_geom_mask & HeightValue) {
		if(flipxy) {
			width[DST] = magx * height[SRC];
			copy_from_src_mask |= WidthValue;
		}
		else {
			height[DST] = magy * height[SRC];
			copy_from_src_mask |= HeightValue;
		}
	}

	if(copy_from_src_mask & dest_geom_mask) {
		fprintf(stderr, "Conflicting dimensions between source and dest geometry\n");
		Usage();
	}

	scr = DefaultScreenOfDisplay(dpy);

	depth = DefaultDepthOfScreen(scr);
	if (depth < 8) {
		fprintf(stderr, "%s: need at least 8 bits/pixel\n", progname);
		exit(1);
	}

	if(source_geom_mask & XNegative)
		xgrab += WidthOfScreen(scr);

	if(source_geom_mask & YNegative)
		ygrab += HeightOfScreen(scr);

	if(dest_geom_mask & XNegative)
		xpos += WidthOfScreen(scr);

	if(source_geom_mask & YNegative)
		ypos += HeightOfScreen(scr);

	/* printf("=%dx%d+%d+%d\n", width[DST], height[DST], xpos, ypos); */

	xswa.event_mask = ButtonPressMask|ButtonReleaseMask|ButtonMotionMask;
	xswa.event_mask |= StructureNotifyMask;	/* resize etc.. */
	xswa.event_mask |= KeyPressMask|KeyReleaseMask;		/* commands */
	xswa.background_pixel = BlackPixelOfScreen(scr);

	win = XCreateWindow(dpy, RootWindowOfScreen(scr),
	    xpos, ypos, width[DST], height[DST], 0,
	    DefaultDepthOfScreen(scr), InputOutput,
	    DefaultVisualOfScreen(scr),
	    CWEventMask | CWBackPixel, &xswa);

	XChangeProperty(dpy, win, XA_WM_ICON_NAME, XA_STRING, 8,
			PropModeReplace,
			(unsigned char *)progname, strlen(progname));

	/*
	XChangeProperty(dpy, win, XA_WM_NAME, XA_STRING, 8,
			PropModeReplace,
			(unsigned char *)progname, strlen(progname));
	*/


 	/***	20020213
		code added by <tmancill@debian.org> to handle
		window manager "close" event
	***/
	wm_delete_window = XInternAtom (dpy, "WM_DELETE_WINDOW", False);
	wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
        status = XSetWMProtocols(dpy, win, &wm_delete_window, 1);

	set_title = True;

	status = XMapWindow(dpy, win);

	gcv.plane_mask = AllPlanes;
	gcv.subwindow_mode = IncludeInferiors;
	gcv.function = GXcopy;
	gcv.foreground = WhitePixelOfScreen(scr);
	gcv.background = BlackPixelOfScreen(scr);
	gc = XCreateGC(dpy, RootWindowOfScreen(scr),
		GCFunction|GCPlaneMask|GCSubwindowMode|GCForeground|GCBackground,
		&gcv);

#ifdef FRAME
	gcv.foreground = AllPlanes;
	gcv.plane_mask = WhitePixelOfScreen(scr)^BlackPixelOfScreen(scr);
	gcv.subwindow_mode = IncludeInferiors;
	gcv.function = GXxor;
	framegc = XCreateGC(dpy, RootWindowOfScreen(scr),
		GCFunction|GCPlaneMask|GCSubwindowMode|GCForeground,
		&gcv);
#endif

#ifdef TIMER
	font = XLoadFont(dpy, "fixed");
#endif

	resize(width[DST], height[DST]);

#ifdef FRAME

	{
		static char bitmap_data[] = { 0 };
		static XColor col = { 0 };
		Pixmap curs = XCreatePixmapFromBitmapData(dpy,
			RootWindowOfScreen(scr), bitmap_data, 1, 1, 0, 0, 1);

		when_button = XCreatePixmapCursor(dpy, curs, curs, &col, &col, 0, 0);
	}
#else
	when_button = XCreateFontCursor(dpy, XC_ul_angle);
#endif
	crosshair = XCreateFontCursor(dpy, XC_crosshair);

	XDefineCursor(dpy, win, crosshair);

	for(;;) {

		for (i = 0; i < number_of_screens; i++) {
			result = XQueryPointer(display, root_windows[i], &window_returned,
					&window_returned, &root_x, &root_y, &win_x, &win_y,
					&mask_return);
			if (result == True) {
				break;
			}
		}
		if (result != True) {
			fprintf(stderr, "No mouse found.\n");
			return -1;
		}
		//printf("Mouse is at (%d,%d)\n", root_x, root_y);
		xgrab = root_x - width[SRC]/2;
		ygrab = root_y - height[SRC]/2;
		/*****
		old event loop updated to support WM messages
		while(unmapped?
			(XWindowEvent(dpy, win, (long)-1, &event), 1):
			XCheckWindowEvent(dpy, win, (long)-1, &event)) {
		******/

		while(XPending(dpy)) {
			XNextEvent(dpy, &event);
			switch(event.type) {
			case ClientMessage:
                        	if ((event.xclient.message_type == wm_protocols) &&
                                    (event.xclient.data.l[0] == wm_delete_window)) {
                                	exit(0);
                        	}
                        	break;
			case ConfigureNotify:
				if(event.xconfigure.width != width[DST] ||
				   event.xconfigure.height != height[DST]) {

					resize(event.xconfigure.width, event.xconfigure.height);
				}
				break;
			case ReparentNotify:
				break;	/* what do we do with it? */

			case MapNotify:
				unmapped = False;
				break;

			case UnmapNotify:
				unmapped = True;
				break;

			case KeyRelease:
				switch(XKeycodeToKeysym(dpy, event.xkey.keycode, 0)) {
				case XK_Control_L:
				case XK_Control_R:
					scroll = 1;
					break;
				}
				break;

			case KeyPress:
				switch(XKeycodeToKeysym(dpy, event.xkey.keycode, 0)) {
				case XK_Control_L:
				case XK_Control_R:
					scroll = 10;
					break;

				case '+':
				case '=':
				case XK_KP_Add:
					if(!yzoom_flag) ++magx;
					if(!xzoom_flag) ++magy;
					xzoom_flag = yzoom_flag = False;
					resize(width[DST], height[DST]);
					set_title = True;
					break;

				case '-':
				case XK_KP_Subtract:
					if(!yzoom_flag) --magx;
					if(!xzoom_flag) --magy;
					xzoom_flag = yzoom_flag = False;
					if(magx < 1) magx = 1;
					if(magy < 1) magy = 1;
					resize(width[DST], height[DST]);
					set_title = True;
					break;

				case XK_Left:
				case XK_KP_Left:
					if(flipxy)
						if(flipx)
							ygrab += scroll;
						else
							ygrab -= scroll;
					else
						if(flipx)
							xgrab += scroll;
						else
							xgrab -= scroll;
					break;

				case XK_Right:
				case XK_KP_Right:
					if(flipxy)
						if(flipx)
							ygrab -= scroll;
						else
							ygrab += scroll;
					else
						if(flipx)
							xgrab -= scroll;
						else
							xgrab += scroll;
					break;

				case XK_Up:
				case XK_KP_Up:
					if(flipxy)
						if(flipy)
							xgrab -= scroll;
						else
							xgrab += scroll;
					else
						if(flipy)
							ygrab += scroll;
						else
							ygrab -= scroll;
					break;

				case XK_Down:
				case XK_KP_Down:
					if(flipxy)
						if(flipy)
							xgrab += scroll;
						else
							xgrab -= scroll;
					else
						if(flipy)
							ygrab -= scroll;
						else
							ygrab += scroll;
					break;

				case 'x':
					flipx = !flipx;
					set_title = True;
					break;

				case 'y':
					flipy = !flipy;
					set_title = True;
					break;

				case 'z':
					if(flipx^flipy^flipxy) {
						flipx = !flipx;
						flipy = !flipy;
					}
					flipxy = !flipxy;
					resize(width[DST], height[DST]);
					set_title = True;
					break;

				case 'w':
					xzoom_flag = True;
					yzoom_flag = False;
					break;

				case 'h':
					yzoom_flag = True;
					xzoom_flag = False;
					break;

				case 'g':
					gridx = !gridx;
					gridy = !gridy;
					break;

				case 'd':
					if(++delay_index >= NDELAYS)
						delay_index = 0;
					delay = delays[delay_index];
					sprintf(title, "delay = %d ms", delay/1000);
					XChangeProperty(dpy, win, XA_WM_NAME, XA_STRING, 8,
						PropModeReplace,
						(unsigned char *)title, strlen(title));
					signal(SIGALRM, timeout_func);
					alarm(2);
					break;

				case 'q':
					free(root_windows);
					XCloseDisplay(display);
					exit(0);
					break;
				}

				break;

			case ButtonPress:
#ifdef FRAME
				xgrab = event.xbutton.x_root - width[SRC]/2;
				ygrab = event.xbutton.y_root - height[SRC]/2;
#else
				xgrab = event.xbutton.x_root;
				ygrab = event.xbutton.y_root;
#endif
				XDefineCursor(dpy, win, when_button);
				buttonpressed = True;
				break;

			case ButtonRelease:
				/*
				xgrab = event.xbutton.x_root - width[SRC]/2;
				ygrab = event.xbutton.y_root - height[SRC]/2;
				*/
				XDefineCursor(dpy, win, crosshair);
				buttonpressed = False;
				break;

			case MotionNotify:
				if(buttonpressed) {
#ifdef FRAME
					xgrab = event.xmotion.x_root - width[SRC]/2;
					ygrab = event.xmotion.y_root - height[SRC]/2;
#else
					xgrab = event.xmotion.x_root;
					ygrab = event.xmotion.y_root;
#endif
				}
				break;

			}

			/* trying XShmGetImage when part of the rect is
			   not on the screen will fail LOUDLY..
			   we have to veryfy this after anything that may
			   may modified xgrab or ygrab or the size of
			   the source ximage */

			if(xgrab < 0)
				xgrab = 0;

			if(xgrab > WidthOfScreen(scr)-width[SRC])
				xgrab =  WidthOfScreen(scr)-width[SRC];

			if(ygrab < 0)
				ygrab = 0;

			if(ygrab > HeightOfScreen(scr)-height[SRC])
				ygrab = HeightOfScreen(scr)-height[SRC];

		}

#ifdef XSHM
		XShmGetImage(dpy, RootWindowOfScreen(scr), ximage[SRC],
			xgrab, ygrab, AllPlanes);
#else
		XGetSubImage(dpy, RootWindowOfScreen(scr),
			xgrab, ygrab, width[SRC], height[SRC], AllPlanes,
			ZPixmap, ximage[SRC], 0, 0);
#endif
#ifdef FRAME
		if(buttonpressed) {	/* show the frame */
			DRAW_FRAME();
			XSync(dpy, False);
		}
#endif

		if (depth == 8)
			scale8();
		else if (depth <= 8*sizeof(short))
			scale16();
		else if (depth <= 8*sizeof(int))
			scale32();

#ifdef XSHM
		XShmPutImage(dpy, win, gc, ximage[DST], 0, 0, 0, 0, width[DST], height[DST], False);
#else
		XPutImage(dpy, win, gc, ximage[DST], 0, 0, 0, 0, width[DST], height[DST]);
#endif
		if(set_title) {
			if(magx == magy && !flipx && !flipy && !flipxy)
				sprintf(title, "%s x%d", progname, magx);
			else
				sprintf(title, "%s X %s%d%s Y %s%d",
					progname,
						flipx?"-":"", magx,
						flipxy?" <=>":";",
						flipy?"-":"", magy);
			XChangeProperty(dpy, win, XA_WM_NAME, XA_STRING, 8,
				PropModeReplace,
				(unsigned char *)title, strlen(title));
			set_title = False;
		}
#ifdef TIMER
		{
			struct timeval current_time;
			double DT;

			gettimeofday(&current_time, NULL);
			DT = current_time.tv_sec - old_time.tv_sec;
			DT += 1e-6*(current_time.tv_usec - old_time.tv_usec);
			sprintf(title, "DT=%6.3f", DT);
			XDrawString(dpy, win, gc, 20, 20, title, strlen(title));
			old_time = current_time;
		}
#endif
		XSync(dpy, 0);

#ifdef NO_USLEEP
#define usleep(_t)								\
	{											\
		struct timeval timeout;					\
		timeout.tv_sec =  0;					\
		timeout.tv_usec = _t;					\
		select(0, NULL, NULL, NULL, &timeout);	\
	}
#endif

		if(!buttonpressed && delay > 0)
			usleep(delay);
#ifdef FRAME
		if(buttonpressed)	/* erase the frame */
			DRAW_FRAME();
#endif
	}
}
