/*
    SDL - Simple DirectMedia Layer
    Copyright (C) 1997, 1998, 1999, 2000, 2001, 2002  Sam Lantinga

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public
    License along with this library; if not, write to the Free
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

    Sam Lantinga
    slouken@libsdl.org
*/
#include "SDL_config.h"

/* This is the DirectDraw implementation of YUV video overlays */

#include "SDL_video.h"
#include "SDL_dx5yuv_c.h"
#include "../SDL_yuvfuncs.h"

#define USE_DIRECTX_OVERLAY

/* The functions used to manipulate software video overlays */
static struct private_yuvhwfuncs dx5_yuvfuncs = {
	DX5_LockYUVOverlay,
	DX5_UnlockYUVOverlay,
	DX5_DisplayYUVOverlay,
	DX5_FreeYUVOverlay
};

struct private_yuvhwdata {
	LPDIRECTDRAWSURFACE3 surface;
	LPDIRECTDRAWSURFACE3 surface_back;	// MATT added (back buffer)
	DWORD	overlay_flags;	// MATT added (will contain DDOVER_SHOW at minimum)
	DDOVERLAYFX          OverlayFX;	// MATT added (for key color)

	/* These are just so we don't have to allocate them separately */
	Uint16 pitches[3];
	Uint8 *planes[3];
};

void DX5_Update_Overlay();	// MPO

int g_overlay_needs_updating = 0;	// MATT, so we can receive external windows events
// like WM_PAINT, WM_MOVE, etc ...

static LPDIRECTDRAWSURFACE3 CreateYUVSurface(_THIS,
                                         int width, int height, Uint32 format)
{
	HRESULT result;
	LPDIRECTDRAWSURFACE  dd_surface1;
	LPDIRECTDRAWSURFACE3 dd_surface3;
	DDSURFACEDESC ddsd;
	unsigned int uAttempts = 0;	// MPO, to ensure we don't loop endlessly trying to get a lock

	/* Set up the surface description */
	memset(&ddsd, 0, sizeof(ddsd));
	ddsd.dwSize = sizeof(ddsd);

	// MATT : added DDSD_BACKBUFFERCOUNT
	ddsd.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_CAPS | DDSD_PIXELFORMAT |
					DDSD_BACKBUFFERCOUNT;

	ddsd.dwBackBufferCount = 1;	// MATT : added a back buffer

	ddsd.dwWidth = width;
	ddsd.dwHeight= height;
#ifdef USE_DIRECTX_OVERLAY
	// MATT : added FLIP and COMPLEX to support the backbuffer
	ddsd.ddsCaps.dwCaps = (DDSCAPS_OVERLAY | DDSCAPS_VIDEOMEMORY | 
		DDSCAPS_FLIP | DDSCAPS_COMPLEX);
#else
	ddsd.ddsCaps.dwCaps = (DDSCAPS_OFFSCREENPLAIN|DDSCAPS_VIDEOMEMORY);
#endif
	ddsd.ddpfPixelFormat.dwSize = sizeof(ddsd.ddpfPixelFormat);
	ddsd.ddpfPixelFormat.dwFlags = DDPF_FOURCC;
	ddsd.ddpfPixelFormat.dwFourCC = format;

	/* Create the DirectDraw video surface */
	result = IDirectDraw2_CreateSurface(ddraw2, &ddsd, &dd_surface1, NULL); 
	if ( result != DD_OK ) {
		SetDDerror("DirectDraw2::CreateSurface", result);
		return(NULL);
	}
	result = IDirectDrawSurface_QueryInterface(dd_surface1,
			&IID_IDirectDrawSurface3, (LPVOID *)&dd_surface3);
	IDirectDrawSurface_Release(dd_surface1);
	if ( result != DD_OK ) {
		SetDDerror("DirectDrawSurface::QueryInterface", result);
		return(NULL);
	}

	/* Make sure the surface format was set properly */
	memset(&ddsd, 0, sizeof(ddsd));
	ddsd.dwSize = sizeof(ddsd);

	// start MPO
	// ATI cards (such as the Radeon 9200) will usually return DDERR_WASSTILLDRAWING
	//  when this Lock is called in fullscreen mode, thus we need to add DDLOCK_WAIT
	
	// spend a reasonable amount of time waiting to get the lock
	while (uAttempts < 2000)
	{
		result = IDirectDrawSurface3_Lock(dd_surface3, NULL,
					  &ddsd, (DDLOCK_NOSYSLOCK|DDLOCK_WAIT), NULL);
		if ( result == DDERR_SURFACELOST )
		{
			IDirectDrawSurface3_Restore(dd_surface3);	// apparently there is only one possible return value here, so no need to check it
			Sleep(1);	// be polite
		}
		// else it's some other unexpected error, so HW acceleration probably is not possible
		else break;
		++uAttempts;
	}
	// stop MPO

	if ( result != DD_OK ) {
		SetDDerror("DirectDrawSurface3::Lock", result);
		IDirectDrawSurface_Release(dd_surface3);
		return(NULL);
	}
	IDirectDrawSurface3_Unlock(dd_surface3, NULL);

	if ( !(ddsd.ddpfPixelFormat.dwFlags & DDPF_FOURCC) ||
	      (ddsd.ddpfPixelFormat.dwFourCC != format) ) {
		SDL_SetError("DDraw didn't use requested FourCC format");
		IDirectDrawSurface_Release(dd_surface3);
		return(NULL);
	}

	/* We're ready to go! */
	return(dd_surface3);
}

#ifdef DEBUG_YUV
static char *PrintFOURCC(Uint32 code)
{
	static char buf[5];

	buf[3] = code >> 24;
	buf[2] = (code >> 16) & 0xFF;
	buf[1] = (code >> 8) & 0xFF;
	buf[0] = (code & 0xFF);
	return(buf);
}
#endif

SDL_Overlay *DX5_CreateYUVOverlay(_THIS, int width, int height, Uint32 format, SDL_Surface *display)
{
	SDL_Overlay *overlay;
	struct private_yuvhwdata *hwdata;

	DX5_Update_Overlay();	// MATT
#ifdef DEBUG_YUV
	DWORD numcodes;
	DWORD *codes;

	printf("FOURCC format requested: 0x%x\n", PrintFOURCC(format));
	IDirectDraw2_GetFourCCCodes(ddraw2, &numcodes, NULL);
	if ( numcodes ) {
		DWORD i;
		codes = SDL_malloc(numcodes*sizeof(*codes));
		if ( codes ) {
			IDirectDraw2_GetFourCCCodes(ddraw2, &numcodes, codes);
			for ( i=0; i<numcodes; ++i ) {
				fprintf(stderr, "Code %d: 0x%x\n", i, PrintFOURCC(codes[i]));
			}
			SDL_free(codes);
		}
	} else {
		fprintf(stderr, "No FOURCC codes supported\n");
	}
#endif

	/* Create the overlay structure */
	overlay = (SDL_Overlay *)SDL_malloc(sizeof *overlay);
	if ( overlay == NULL ) {
		SDL_OutOfMemory();
		return(NULL);
	}
	SDL_memset(overlay, 0, (sizeof *overlay));

	/* Fill in the basic members */
	overlay->format = format;
	overlay->w = width;
	overlay->h = height;

	/* Set up the YUV surface function structure */
	overlay->hwfuncs = &dx5_yuvfuncs;

	/* Create the pixel data and lookup tables */
	hwdata = (struct private_yuvhwdata *)SDL_malloc(sizeof *hwdata);
	overlay->hwdata = hwdata;
	if ( hwdata == NULL ) {
		SDL_OutOfMemory();
		SDL_FreeYUVOverlay(overlay);
		return(NULL);
	}
	hwdata->surface = CreateYUVSurface(this, width, height, format);
	if ( hwdata->surface == NULL ) {
		SDL_FreeYUVOverlay(overlay);
		return(NULL);
	}

	// start MATT
	// I enclosed this in brackets so I could declare new data types in the middle
	// of this function, in order to keep my code closer together.
	{
		DDSCAPS ddsc_attached;	// used to query for our overlay's attached back buffer
		DDCAPS dd_caps;	// used to find out whether we can support color keying
		LPDIRECTDRAWSURFACE3 dd_surface3_attached;	// points to our back buffer if we are successful
		HRESULT hr;	// holds result of our actions

		// get the back buffer of the overlay
		memset( &ddsc_attached, 0, sizeof( DDSCAPS ) );
		ddsc_attached.dwCaps = DDSCAPS_BACKBUFFER;
		hr = IDirectDrawSurface3_GetAttachedSurface(hwdata->surface, &ddsc_attached, &dd_surface3_attached);

		// if we couldn't get the back buffer ...
		if (FAILED(hr))
		{
			SDL_FreeYUVOverlay(overlay);
			return(NULL);
		}

		// store the back buffer so we can use it later without querying again
		hwdata->surface_back = dd_surface3_attached;

	    // Get driver capabilities to determine Overlay support.
	    ZeroMemory( &dd_caps, sizeof(dd_caps) );
	    dd_caps.dwSize = sizeof(dd_caps);

		// query our directdraw capabilities
		// (this might be done elsewhere, but I haven't studied the SDL source enough)
		hr = IDirectDraw2_GetCaps(ddraw2, &dd_caps, NULL);
	    if (FAILED(hr))
		{
			SDL_FreeYUVOverlay(overlay);
			return(NULL);
		}

		ZeroMemory(&hwdata->OverlayFX, sizeof(hwdata->OverlayFX));
		hwdata->OverlayFX.dwSize = sizeof(hwdata->OverlayFX);
		hwdata->overlay_flags = DDOVER_SHOW;	// at minimum we will show the overlay

		// if we can support color keying
		if (dd_caps.dwCKeyCaps & DDCKEYCAPS_DESTOVERLAY)
		{
	        // Using a color key will clip the overlay 
	        // when the mouse or other windows go on top of us. 
	        DWORD dwDDSColor = 0x010101; // our color key color (almost black)

			SDL_FillRect(display, NULL, dwDDSColor);
			SDL_UpdateRect(display, 0, 0, 0, 0);	// make the changes
			// fill the regular surface with our color key color so that the
			// overlay will be visible.
			// FIXME : perhaps change SDL API to allow user to specify which
			// color they want, and fill the surface themselves?

	        hwdata->OverlayFX.dckDestColorkey.dwColorSpaceLowValue  = dwDDSColor;
	        hwdata->OverlayFX.dckDestColorkey.dwColorSpaceHighValue = dwDDSColor;
	        hwdata->overlay_flags |= DDOVER_DDFX | DDOVER_KEYDESTOVERRIDE;			
		}

		// else we cannot support color keys, so the overlay will intrude
		// upon all other windows :)

	}

	// end MATT

	overlay->hw_overlay = 1;

	/* Set up the plane pointers */
	overlay->pitches = hwdata->pitches;
	overlay->pixels = hwdata->planes;
	switch (format) {
	    case SDL_YV12_OVERLAY:
	    case SDL_IYUV_OVERLAY:
		overlay->planes = 3;
		break;
	    default:
		overlay->planes = 1;
		break;
	}

	/* We're all done.. */
	return(overlay);
}

int DX5_LockYUVOverlay(_THIS, SDL_Overlay *overlay)
{
	HRESULT result;
	LPDIRECTDRAWSURFACE3 surface;
	DDSURFACEDESC ddsd;

//	surface = overlay->hwdata->surface;
	surface = overlay->hwdata->surface_back;	// MATT, user should now write to back buffer instead of front buffer
	memset(&ddsd, 0, sizeof(ddsd));
	ddsd.dwSize = sizeof(ddsd);

	result = IDirectDrawSurface3_Lock(surface, NULL, &ddsd, (DDLOCK_NOSYSLOCK|DDLOCK_WAIT), NULL);

	if ( result == DDERR_SURFACELOST ) {
			result = IDirectDrawSurface3_Restore(surface);
			result = IDirectDrawSurface3_Lock(surface, NULL, &ddsd, 
						(DDLOCK_NOSYSLOCK|DDLOCK_WAIT), NULL);
	}

	if ( result != DD_OK ) {
			SetDDerror("DirectDrawSurface3::Lock", result);
			return(-1);
	}

	/* Find the pitch and offset values for the overlay */
#if defined(NONAMELESSUNION)
	overlay->pitches[0] = (Uint16)ddsd.u1.lPitch;
#else
	overlay->pitches[0] = (Uint16)ddsd.lPitch;
#endif
	overlay->pixels[0] = (Uint8 *)ddsd.lpSurface;
	switch (overlay->format) {
	    case SDL_YV12_OVERLAY:
	    case SDL_IYUV_OVERLAY:
		/* Add the two extra planes */
		overlay->pitches[1] = overlay->pitches[0] / 2;
		overlay->pitches[2] = overlay->pitches[0] / 2;
	        overlay->pixels[1] = overlay->pixels[0] +
		                     overlay->pitches[0] * overlay->h;
	        overlay->pixels[2] = overlay->pixels[1] +
		                     overlay->pitches[1] * overlay->h / 2;
	        break;
	    default:
		/* Only one plane, no worries */
		break;
	}
	return(0);
}

void DX5_UnlockYUVOverlay(_THIS, SDL_Overlay *overlay)
{
	LPDIRECTDRAWSURFACE3 surface;

//	surface = overlay->hwdata->surface;
	surface = overlay->hwdata->surface_back;	// MATT, user should access back buffer, not front
	IDirectDrawSurface3_Unlock(surface, NULL);
}

int DX5_DisplayYUVOverlay(_THIS, SDL_Overlay *overlay, SDL_Rect *src, SDL_Rect *dst)
{
	HRESULT result = DD_OK;	// MATT
	LPDIRECTDRAWSURFACE3 surface;
	RECT srcrect, dstrect;

#ifdef USE_DIRECTX_OVERLAY
	static SDL_Rect previous = { 0, 0, 0, 0 };	// MATT :we use this to keep track of whenever
	// the requested dstrect changes.  If the dstrect changes, we have to do an expensive
	// UpdateOverlay call.

	// if dstrect has changed
	if ((dst->w != previous.w) || (dst->h != previous.h) ||
		(dst->x != previous.x) || (dst->y != previous.y))
	{
		previous.x = dst->x;
		previous.y = dst->y;
		previous.w = dst->w;
		previous.h = dst->h;
		g_overlay_needs_updating = 1;	// force UpdateOverlay call
	}

	surface = overlay->hwdata->surface;

	// only update the overlay if we need to because it is very expensive to do so!!
	if (g_overlay_needs_updating)
	{
		// MATT : we only need to make these calculations if we are updating the overlay
		srcrect.top = src->y;
		srcrect.bottom = srcrect.top+src->h;
		srcrect.left = src->x;
		srcrect.right = srcrect.left+src->w;
		dstrect.top = SDL_bounds.top+dst->y;
		dstrect.left = SDL_bounds.left+dst->x;
		dstrect.bottom = dstrect.top+dst->h;
		dstrect.right = dstrect.left+dst->w;

		// this call now allows for color keying
		result = IDirectDrawSurface3_UpdateOverlay(surface, &srcrect,
				SDL_primary, &dstrect, overlay->hwdata->overlay_flags,
				&overlay->hwdata->OverlayFX);

		if ( result != DD_OK )
		{
			SetDDerror("DirectDrawSurface3::UpdateOverlay", result);
			return(-1);
		}
		else
		{
			g_overlay_needs_updating = 0;
		}
	}

	// MATT : flip the overlay buffer to display the new contents
	result = IDirectDrawSurface3_Flip(surface, NULL, DDFLIP_WAIT);
	if (result != DD_OK)
	{
		SetDDerror("DirectDrawSurface3::Flip", result);
	}

#else
	srcrect.top = src->y;
	srcrect.bottom = srcrect.top+src->h;
	srcrect.left = src->x;
	srcrect.right = srcrect.left+src->w;
	dstrect.top = SDL_bounds.top+dst->y;
	dstrect.left = SDL_bounds.left+dst->x;
	dstrect.bottom = dstrect.top+dst->h;
	dstrect.right = dstrect.left+dst->w;

	result = IDirectDrawSurface3_Blt(SDL_primary, &dstrect, surface, &srcrect,
							DDBLT_WAIT, NULL);
	if ( result != DD_OK ) {
		SetDDerror("DirectDrawSurface3::Blt", result);
		return(-1);
	}
#endif
	return(0);
}

void DX5_FreeYUVOverlay(_THIS, SDL_Overlay *overlay)
{
	struct private_yuvhwdata *hwdata;

	hwdata = overlay->hwdata;
	if ( hwdata ) {
		if ( hwdata->surface ) {
			IDirectDrawSurface_Release(hwdata->surface);
		}
		free(hwdata);
	}
}

// MATT
void DX5_Update_Overlay()
{
	g_overlay_needs_updating = 1;
}
