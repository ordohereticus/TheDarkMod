/*****************************************************************************
                    The Dark Mod GPL Source Code
 
 This file is part of the The Dark Mod Source Code, originally based 
 on the Doom 3 GPL Source Code as published in 2011.
 
 The Dark Mod Source Code is free software: you can redistribute it 
 and/or modify it under the terms of the GNU General Public License as 
 published by the Free Software Foundation, either version 3 of the License, 
 or (at your option) any later version. For details, see LICENSE.TXT.
 
 Project: The Dark Mod (http://www.thedarkmod.com/)
 
******************************************************************************/
#include "../../idlib/precompiled.h"
#include "../../renderer/tr_local.h"
#include "local.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <X11/Xatom.h>

idCVar sys_videoRam( "sys_videoRam", "0", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_INTEGER, "Texture memory on the video card (in megabytes) - 0: autodetect", 0, 512 );
idCVar v_nowmfullscreen( "v_nowmfullscreen", "0", CVAR_SYSTEM | CVAR_ARCHIVE | CVAR_NOCHEAT, "Do not use the window manager for fullscreen. If this is set and full screen is used, it will not notify the window manager and will monopolize both mouse and keyboard inputs. Only used at screen initialization.");

Display *dpy = NULL;
int scrnum = 0;

Window win = 0;

bool dga_found = false;

static GLXContext ctx = NULL;

static bool vidmode_ext = false;
static int vidmode_MajorVersion = 0, vidmode_MinorVersion = 0;	// major and minor of XF86VidExtensions

static XF86VidModeModeInfo **vidmodes;
static int num_vidmodes;
static bool vidmode_active = false;

// Set to true if fullscreen mode was set without asking the window manager to treat this window as full screen.
bool vidmode_nowmfullscreen = false;

// backup gamma ramp
static int save_rampsize = 0;
static unsigned short *save_red, *save_green, *save_blue;

void GLimp_WakeBackEnd(void *a) {
	common->DPrintf("GLimp_WakeBackEnd stub\n");
}

void GLimp_FrontEndSleep() {
	common->DPrintf("GLimp_FrontEndSleep stub\n");
}

void *GLimp_BackEndSleep() {
	common->DPrintf("GLimp_BackEndSleep stub\n");
	return 0;
}

bool GLimp_SpawnRenderThread(void (*a) ()) {
	common->DPrintf("GLimp_SpawnRenderThread stub\n");
	return false;
}

void GLimp_ActivateContext() {
	assert( dpy );
	assert( ctx );
	qglXMakeCurrent( dpy, win, ctx );
}

void GLimp_DeactivateContext() {
	assert( dpy );
	qglXMakeCurrent( dpy, None, NULL );
}

/*
=================
GLimp_SaveGamma

save and restore the original gamma of the system
=================
*/
void GLimp_SaveGamma() {
	if ( save_rampsize ) {
		return;
	}

	assert( dpy );
	
	XF86VidModeGetGammaRampSize( dpy, scrnum, &save_rampsize);
	save_red = (unsigned short *)malloc(save_rampsize*sizeof(unsigned short));
	save_green = (unsigned short *)malloc(save_rampsize*sizeof(unsigned short));
	save_blue = (unsigned short *)malloc(save_rampsize*sizeof(unsigned short));
	XF86VidModeGetGammaRamp( dpy, scrnum, save_rampsize, save_red, save_green, save_blue);
}

/*
=================
GLimp_RestoreGamma

save and restore the original gamma of the system
=================
*/
void GLimp_RestoreGamma() {
	if (!save_rampsize)
		return;
	
	XF86VidModeSetGammaRamp( dpy, scrnum, save_rampsize, save_red, save_green, save_blue);
	
	free(save_red); free(save_green); free(save_blue);
	save_rampsize = 0;
}

/*
=================
GLimp_SetGamma

gamma ramp is generated by the renderer from r_gamma and r_brightness for 256 elements
the size of the gamma ramp can not be changed on X (I need to confirm this)
=================
*/
void GLimp_SetGamma(unsigned short red[256], unsigned short green[256], unsigned short blue[256]) {
	//stgatilov: brightness and gamma adjustments are done in final shader pass
	return;

	if ( dpy ) {		
		int size;
		
		GLimp_SaveGamma();
		XF86VidModeGetGammaRampSize( dpy, scrnum, &size);
		common->DPrintf("XF86VidModeGetGammaRampSize: %d\n", size);
		if ( size > 256 ) {
			// silly generic resample
			int i;
			unsigned short *l_red, *l_green, *l_blue;
			l_red = (unsigned short *)malloc(size*sizeof(unsigned short));
			l_green = (unsigned short *)malloc(size*sizeof(unsigned short));
			l_blue = (unsigned short *)malloc(size*sizeof(unsigned short));
			//int r_size = 256;
			int r_i; float r_f;
			for(i=0; i<size-1; i++) {
				r_f = (float)i*255.0f/(float)(size-1);
				r_i = (int)floor(r_f);
				r_f -= (float)r_i;
				l_red[i] = (int)round((1.0f-r_f)*(float)red[r_i]+r_f*(float)red[r_i+1]);
				l_green[i] = (int)round((1.0f-r_f)*(float)green[r_i]+r_f*(float)green[r_i+1]);
				l_blue[i] = (int)round((1.0f-r_f)*(float)blue[r_i]+r_f*(float)blue[r_i+1]);				
			}
			l_red[size-1] = red[255]; l_green[size-1] = green[255]; l_blue[size-1] = blue[255];
			XF86VidModeSetGammaRamp( dpy, scrnum, size, l_red, l_green, l_blue );
			free(l_red); free(l_green); free(l_blue);
		} else {
			XF86VidModeSetGammaRamp( dpy, scrnum, size, red, green, blue );
		}
	}
}

void GLimp_Shutdown() {
	if ( dpy ) {
		
		Sys_XUninstallGrabs();
	
		GLimp_RestoreGamma();

		if (ctx && qglXDestroyContext) {
			qglXDestroyContext( dpy, ctx );
		}

		common->Printf( "...shutting down QGL\n" );
		GLimp_UnloadFunctions();
		
		XDestroyWindow( dpy, win );
		if ( vidmode_active ) {
			XF86VidModeSwitchToMode( dpy, scrnum, vidmodes[0] );
		}

		XFlush( dpy );

		// FIXME: that's going to crash
		XCloseDisplay( dpy );

		vidmode_active = false;
		dpy = NULL;
		win = 0;
		ctx = NULL;
	}
}

void GLimp_SwapBuffers() {
	assert( dpy );
	qglXSwapBuffers( dpy, win );
}

/*
GLX_TestDGA
Check for DGA	- update in_dgamouse if needed
*/
void GLX_TestDGA() {

#if defined( ID_ENABLE_DGA )
	int dga_MajorVersion = 0, dga_MinorVersion = 0;
	assert( dpy );
	if ( !XF86DGAQueryVersion( dpy, &dga_MajorVersion, &dga_MinorVersion ) ) {
		// unable to query, probalby not supported
		common->Printf( "Failed to detect DGA DirectVideo Mouse\n" );
		cvarSystem->SetCVarBool( "in_dgamouse", false );
		dga_found = false;
	} else {
		common->Printf( "DGA DirectVideo Mouse (Version %d.%d) initialized\n",
				   dga_MajorVersion, dga_MinorVersion );
		dga_found = true;
	}
#else
    dga_found = false;
#endif
}

/*
** XErrorHandler
**   the default X error handler exits the application
**   I found out that on some hosts some operations would raise X errors (GLXUnsupportedPrivateRequest)
**   but those don't seem to be fatal .. so the default would be to just ignore them
**   our implementation mimics the default handler behaviour (not completely cause I'm lazy)
*/
int idXErrorHandler(Display * l_dpy, XErrorEvent * ev) {
	char buf[1024];
	common->Printf( "Fatal X Error:\n" );
	common->Printf( "  Major opcode of failed request: %d\n", ev->request_code );
	common->Printf( "  Minor opcode of failed request: %d\n", ev->minor_code );
	common->Printf( "  Serial number of failed request: %lu\n", ev->serial );
	XGetErrorText( l_dpy, ev->error_code, buf, 1024 );
	common->Printf( "%s\n", buf );
	return 0;
}

bool GLimp_OpenDisplay( void ) {
	if ( dpy ) {
		return true;
	}

	if ( cvarSystem->GetCVarInteger( "net_serverDedicated" ) == 1 ) {
		common->DPrintf( "not opening the display: dedicated server\n" );
		return false;
	}

	common->Printf( "Setup X display connection\n" );

	// that should be the first call into X
	if ( !XInitThreads() ) {
		common->Printf("XInitThreads failed\n");
		return false;
	}
	
	// set up our custom error handler for X failures
	XSetErrorHandler( &idXErrorHandler );

	if ( !( dpy = XOpenDisplay(NULL) ) ) {
		common->Printf( "Couldn't open the X display\n" );
		return false;
	}
	scrnum = DefaultScreen( dpy );

	common->Printf( "Using screen %d of %p display\n", scrnum, dpy );
	return true;
}

/*
===============
GLX_Init
===============
*/
int GLX_Init(glimpParms_t a) {
	if ( !GLimp_OpenDisplay() ) {
		return false;
	}

	//load basic functions like glXCreateContext
	GLimp_LoadFunctions(false);

	common->Printf( "Initializing OpenGL display\n" );

	if (!GLAD_GLX_ARB_create_context || !GLAD_GLX_ARB_create_context_profile) {
		common->Printf("Missing GLX extensions required to create GL3+ contexts\n");
		return false;
	}

	Window root = RootWindow( dpy, scrnum );

	int actualWidth = glConfig.vidWidth;
	int actualHeight = glConfig.vidHeight;

	// Get video mode list
	if ( !XF86VidModeQueryVersion( dpy, &vidmode_MajorVersion, &vidmode_MinorVersion ) ) {
		vidmode_ext = false;
		common->Printf("XFree86-VidModeExtension not available\n");
	} else {
		vidmode_ext = true;
		common->Printf("Using XFree86-VidModeExtension Version %d.%d\n",
				   vidmode_MajorVersion, vidmode_MinorVersion);
	}

	GLX_TestDGA();

	if ( vidmode_ext ) {
		int best_fit, best_dist, dist, x, y;

		XF86VidModeGetAllModeLines( dpy, scrnum, &num_vidmodes, &vidmodes );

		// Are we going fullscreen?  If so, let's change video mode
		if ( a.fullScreen ) {
			best_dist = 9999999;
			best_fit = -1;

			for (int i = 0; i < num_vidmodes; i++) {
				if (a.width > vidmodes[i]->hdisplay ||
					a.height > vidmodes[i]->vdisplay)
					continue;

				x = a.width - vidmodes[i]->hdisplay;
				y = a.height - vidmodes[i]->vdisplay;
				dist = (x * x) + (y * y);
				if (dist < best_dist) {
					best_dist = dist;
					best_fit = i;
				}
			}

			if (best_fit != -1) {
				actualWidth = vidmodes[best_fit]->hdisplay;
				actualHeight = vidmodes[best_fit]->vdisplay;

				// change to the mode
				XF86VidModeSwitchToMode(dpy, scrnum, vidmodes[best_fit]);
				vidmode_active = true;

				// Move the viewport to top left
				// FIXME: center?
				XF86VidModeSetViewPort(dpy, scrnum, 0, 0);

				common->Printf( "Free86-VidModeExtension Activated at %dx%d\n", actualWidth, actualHeight );

			} else {
				a.fullScreen = false;
				common->Printf( "Free86-VidModeExtension: No acceptable modes found\n" );
			}
		} else {
			common->Printf( "XFree86-VidModeExtension: not fullscreen, ignored\n" );
		}
	}

	GLXFBConfig bestFbc = 0;
	XVisualInfo *visinfo = NULL;

	if (r_glCoreProfile.GetInteger() == 0) {
		//stgatilov: old code using deprecated glXChooseVisual and glXCreateContext
		//should be removed completely by TDM 2.09

		int attrib[] = {
			GLX_RGBA,				// 0
			GLX_RED_SIZE, 8,		// 1, 2
			GLX_GREEN_SIZE, 8,		// 3, 4
			GLX_BLUE_SIZE, 8,		// 5, 6
			GLX_DOUBLEBUFFER,		// 7
			GLX_DEPTH_SIZE, 24,		// 8, 9
			GLX_STENCIL_SIZE, 8,	// 10, 11
			GLX_ALPHA_SIZE, 8, // 12, 13
			None
		};
		// these match in the array
		#define ATTR_RED_IDX 2
		#define ATTR_GREEN_IDX 4
		#define ATTR_BLUE_IDX 6
		#define ATTR_DEPTH_IDX 9
		#define ATTR_STENCIL_IDX 11
		#define ATTR_ALPHA_IDX 13

		// color, depth and stencil
		int colorbits = 24;
		int depthbits = 24;
		int stencilbits = 8;

		for (int i = 0; i < 16; i++) {
			// 0 - default
			// 1 - minus colorbits
			// 2 - minus depthbits
			// 3 - minus stencil
			if ((i % 4) == 0 && i) {
				// one pass, reduce
				switch (i / 4) {
				case 2:
					if (colorbits == 24)
						colorbits = 16;
					break;
				case 1:
					if (depthbits == 24)
						depthbits = 16;
					else if (depthbits == 16)
						depthbits = 8;
				case 3:
					if (stencilbits == 24)
						stencilbits = 16;
					else if (stencilbits == 16)
						stencilbits = 8;
				}
			}

			int tcolorbits = colorbits;
			int tdepthbits = depthbits;
			int tstencilbits = stencilbits;

			if ((i % 4) == 3) {		// reduce colorbits
				if (tcolorbits == 24)
					tcolorbits = 16;
			}

			if ((i % 4) == 2) {		// reduce depthbits
				if (tdepthbits == 24)
					tdepthbits = 16;
				else if (tdepthbits == 16)
					tdepthbits = 8;
			}

			if ((i % 4) == 1) {		// reduce stencilbits
				if (tstencilbits == 24)
					tstencilbits = 16;
				else if (tstencilbits == 16)
					tstencilbits = 8;
				else
					tstencilbits = 0;
			}

			if (tcolorbits == 24) {
				attrib[ATTR_RED_IDX] = 8;
				attrib[ATTR_GREEN_IDX] = 8;
				attrib[ATTR_BLUE_IDX] = 8;
			} else {
				// must be 16 bit
				attrib[ATTR_RED_IDX] = 4;
				attrib[ATTR_GREEN_IDX] = 4;
				attrib[ATTR_BLUE_IDX] = 4;
			}
			
			attrib[ATTR_DEPTH_IDX] = tdepthbits;	// default to 24 depth
			attrib[ATTR_STENCIL_IDX] = tstencilbits;

			visinfo = qglXChooseVisual(dpy, scrnum, attrib);
			if (!visinfo) {
				continue;
			}

			common->Printf( "Using %d/%d/%d Color bits, %d Alpha bits, %d depth, %d stencil display.\n",
				 attrib[ATTR_RED_IDX], attrib[ATTR_GREEN_IDX],
				 attrib[ATTR_BLUE_IDX], attrib[ATTR_ALPHA_IDX],
				 attrib[ATTR_DEPTH_IDX],
				 attrib[ATTR_STENCIL_IDX]);

			break;
		}

		if (!visinfo) {
			common->Printf("Couldn't get a visual\n");
			return false;
		}

	}
	else {
		//stgatilov: new code using qglXChooseFBConfig and qglXCreateContextAttribsARB
		//supports GL3+ and choosing core profile

		int config_attribs[] = {
			GLX_X_RENDERABLE  , True,              //have associated X visuals
			GLX_DRAWABLE_TYPE , GLX_WINDOW_BIT,    //window, not pixmap/pbuffer
			GLX_RENDER_TYPE   , GLX_RGBA_BIT,      //render RGBA, not color index
			GLX_X_VISUAL_TYPE , GLX_TRUE_COLOR,    //???
			GLX_RED_SIZE      , 8,
			GLX_GREEN_SIZE    , 8,
			GLX_BLUE_SIZE     , 8,
			GLX_ALPHA_SIZE    , 8,
			GLX_DEPTH_SIZE    , 24,
			GLX_STENCIL_SIZE  , 8,
			GLX_DOUBLEBUFFER  , True,
			GLX_SAMPLE_BUFFERS, 0,                  //no multisampling
			GLX_SAMPLES       , 0,                  //no multisampling
			None
		};
		int fbcount;
		GLXFBConfig *fbc = qglXChooseFBConfig( dpy, scrnum, config_attribs, &fbcount );

		int best_fbc = -1;
		for (int i = 0; i<fbcount; ++i) {
			XVisualInfo *vi = qglXGetVisualFromFBConfig( dpy, fbc[i] );
			if ( vi && best_fbc < 0 )
				best_fbc = i;
			XFree( vi );
		}
		if (best_fbc < 0) {
			common->Printf("Couldn't choose FBConfig\n");
			return false;
		}
		bestFbc = fbc[ best_fbc ];
		visinfo = qglXGetVisualFromFBConfig( dpy, bestFbc );
		common->Printf("Chosen visual: 0x%03x\n", (int)XVisualIDFromVisual(visinfo->visual));
	}

	// window attributes
	XSetWindowAttributes attr;
	attr.background_pixel = BlackPixel(dpy, scrnum);
	attr.border_pixel = 0;
	attr.colormap = XCreateColormap(dpy, root, visinfo->visual, AllocNone);
	attr.event_mask = X_MASK;
	unsigned long mask;
	if (vidmode_active) {
		if(v_nowmfullscreen.GetBool()) {
			/*We're not going to cooperate with any window managers, so the window will grab full control.*/
			mask = CWBackPixel | CWColormap | CWSaveUnder | CWBackingStore |
				CWEventMask | CWOverrideRedirect | CWBorderPixel;
			vidmode_nowmfullscreen = true;
			attr.override_redirect = True;
		} else {
			/*Create a normal window and later inform the window manager that this is fullscreen.*/
			mask = CWBackPixel | CWColormap | CWSaveUnder | CWBackingStore |
				CWEventMask | CWBorderPixel;
			vidmode_nowmfullscreen = false;
		}
		attr.backing_store = NotUseful;
		attr.save_under = False;
	} else {
		mask = CWBackPixel | CWBorderPixel | CWColormap | CWEventMask;
		vidmode_nowmfullscreen = false;
	}

	win = XCreateWindow(dpy, root, 0, 0,
						actualWidth, actualHeight,
						0, visinfo->depth, InputOutput,
						visinfo->visual, mask, &attr);

	if(vidmode_active && !vidmode_nowmfullscreen) {
		/*Tell the window manager that this is a full screen window.*/
		const Atom atoms[2] = { XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False), None };
		XChangeProperty(
			dpy, 
			win, 
			XInternAtom(dpy, "_NET_WM_STATE", False),
			XA_ATOM, 32, PropModeReplace, (const unsigned char *)atoms, 1
			);
	}
	XStoreName(dpy, win, GAME_NAME);

	// don't let the window be resized
	// FIXME: allow resize (win32 does)
	XSizeHints sizehints;
	sizehints.flags = PMinSize | PMaxSize;
	sizehints.min_width = sizehints.max_width = actualWidth;
	sizehints.min_height = sizehints.max_height = actualHeight;

	XSetWMNormalHints(dpy, win, &sizehints);

	XMapWindow( dpy, win );

	if ( vidmode_active ) {
		XMoveWindow( dpy, win, 0, 0 );
	}

	XFlush(dpy);
	XSync(dpy, False);
	if (r_glCoreProfile.GetInteger() == 0 || !(GLAD_GLX_ARB_create_context && GLAD_GLX_ARB_create_context_profile)) {
		r_glCoreProfile.SetInteger(0);
		common->Printf( "...creating GL context: deprecated\n" );
		ctx = qglXCreateContext(dpy, visinfo, NULL, True);    //old-style context creation
	}
	else {
		common->Printf( "...creating GL context: " );
		if( r_glCoreProfile.GetInteger() == 0 )
			common->Printf("compatibility ");
		else if( r_glCoreProfile.GetInteger() == 1 )
			common->Printf("core ");
		else if( r_glCoreProfile.GetInteger() == 2 )
			common->Printf("core-fc ");
		if( r_glDebugContext.GetBool() )
			common->Printf("debug ");
		common->Printf("\n");
		int context_attribs[] = {
			GLX_CONTEXT_MAJOR_VERSION_ARB, QGL_REQUIRED_VERSION_MAJOR,
			GLX_CONTEXT_MINOR_VERSION_ARB, QGL_REQUIRED_VERSION_MINOR,
			GLX_CONTEXT_PROFILE_MASK_ARB , GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
			GLX_CONTEXT_FLAGS_ARB        , (r_glCoreProfile.GetInteger() > 1 ? GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB : 0) | (r_glDebugContext.GetBool() ? GLX_CONTEXT_DEBUG_BIT_ARB : 0),
			None
		};
		ctx = qglXCreateContextAttribsARB(dpy, bestFbc, NULL, True, context_attribs);
	}
	XSync(dpy, False);
	if (!ctx)
		common->Error("Failed to create OpenGL context");

	// Free the visinfo after we're done with it
	XFree(visinfo);

	if (!qglXMakeCurrent(dpy, win, ctx))
		common->Error("Failed to make created GL context current");

	glConfig.isFullscreen = a.fullScreen;
	
	if ( glConfig.isFullscreen ) {
		Sys_GrabMouseCursor( true );
	}
	
	return true;
}

/*
===================
GLimp_Init

This is the platform specific OpenGL initialization function.  It
is responsible for loading OpenGL, initializing it,
creating a window of the appropriate size, doing
fullscreen manipulations, etc.  Its overall responsibility is
to make sure that a functional OpenGL subsystem is operating
when it returns to the ref.

If there is any failure, the renderer will revert back to safe
parameters and try again.
===================
*/
bool GLimp_Init( glimpParms_t a ) {

	if ( !GLimp_OpenDisplay() ) {
		return false;
	}
	
	if (!GLX_Init(a)) {
		return false;
	}

	common->Printf( "...initializing QGL\n" );
	//load all function pointers available in the final context
	GLimp_LoadFunctions();
	
	return true;
}

/*
===================
GLimp_SetScreenParms
===================
*/
bool GLimp_SetScreenParms( glimpParms_t parms ) {
	return true;
}
