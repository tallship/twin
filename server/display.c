/*
 *  display.c  -- connect to a running twin and register as a display
 *
 *  Copyright (C) 2000 by Massimiliano Ghilardi
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 */

/*
 * this is basically a chopped down version of twin,
 * with the minimum required features to startup a display driver,
 * plus libTw code to talk to twin, register on it as a special display,
 * and forward messages and commands between the display driver and twin.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <signal.h>
#include <errno.h>

#include "twin.h"
#include "hw.h"
#include "hw_private.h"
#include "common.h"
#include "version.h"

#include "libTw.h"

/* HW specific headers */

#ifdef CONF_HW_X11
# include "HW/hw_X11.h"
#endif

#ifdef CONF_HW_DISPLAY
# include "HW/hw_display.h"
#endif

#ifdef CONF_HW_TWIN
# include "HW/hw_twin.h"
#endif

#ifdef CONF_HW_TTY
# include "HW/hw_tty.h"
#endif

#ifdef CONF_HW_GGI
# include "HW/hw_ggi.h"
#endif






static byte *MYname;

#ifdef CONF__MODULES

# ifdef CONF_DESTDIR
#  define TWIN_MODULE_DIR CONF_DESTDIR "/lib/twin/modules/"
# else
#  define TWIN_MODULE_DIR "./"
# endif

static byte *twindir = TWIN_MODULE_DIR;

static byte *ErrStr;

#endif /* CONF__MODULES */


static udat TryScreenWidth, TryScreenHeight;
static udat savedScreenWidth, savedScreenHeight;
static byte ValidVideo;

byte *TWDisplay, *origTWDisplay, *origTERM;

byte nullMIME[TW_MAX_MIMELEN];

#define L 0x55
#define M 0xAA
#define H 0xFF

palette Palette[MAXCOL+1] = {
    /* the default colour table, for VGA+ colour systems */
    {0,0,0}, {0,0,M}, {0,M,0}, {0,M,M}, {M,0,0}, {M,0,M}, {M,M,0}, {M,M,M},
    {L,L,L}, {L,L,H}, {L,H,L}, {L,H,H}, {H,L,L}, {H,L,H}, {H,H,L}, {H,H,H}};

palette defaultPalette[MAXCOL+1] = {
    /* the default colour table, for VGA+ colour systems */
    {0,0,0}, {0,0,M}, {0,M,0}, {0,M,M}, {M,0,0}, {M,0,M}, {M,M,0}, {M,M,M},
    {L,L,L}, {L,L,H}, {L,H,L}, {L,H,H}, {H,L,L}, {H,L,H}, {H,H,L}, {H,H,H}};

#undef H
#undef M
#undef L

static fdlist FdList[5];
static uldat FdSize = 5, FdTop, FdBottom, FdWQueued;
#define LS      FdList[Slot]
#define ls      FdList[slot]

static fd_set save_rfds, save_wfds;
static int max_fds;

static tmsgport TMsgPort = NOID, THelper = NOID;

int (*OverrideSelect)(int n, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) = select;

/*
 * first, utility functions
 */

void Quit(int status);

void NoOp(void) {
}
byte AlwaysFalse(void) {
    return FALSE;
}
byte AlwaysTrue(void) {
    return TRUE;
}

void GetPrivileges(void) {
}

static void OutOfMemory(void) {
    fputs("twdisplay: Out of memory!\n", stderr);
}

byte *CloneStr(byte *s) {
    byte *q;
    uldat len;
    
    if (s) {
	len = 1 + LenStr(s);
	if ((q = AllocMem(len)))
	    CopyMem(s, q, len);
	return q;
    }
    return s;
}

byte *CloneStrL(byte *s, uldat len) {
    byte *q;
    
    if (s && len) {
	if ((q = AllocMem(len+1))) {
	    CopyMem(s, q, len);
	    q[len] = '\0';
	}
	return q;
    }
    return s;
}

INLINE uldat FdListGet(void) {
    if (FdBottom < FdSize)
	return FdBottom;
    return NOSLOT;
}

uldat RegisterRemote(int Fd, void *HandlerData, void (*HandlerIO)(int Fd, void *HandlerData)) {
    uldat Slot, j;
    
    if ((Slot = FdListGet()) == NOSLOT)
	return Slot;
    
    WriteMem(&LS, 0, sizeof(fdlist));
    
    LS.Fd = Fd;
    LS.pairSlot = NOSLOT;
    LS.HandlerData = HandlerData;
    LS.HandlerIO = (void (*)(int, size_t))HandlerIO;
    LS.extern_couldntwrite = FALSE;
    
    if (FdTop <= Slot)
	FdTop = Slot + 1;
    for (j = FdBottom + 1; j < FdTop; j++)
	if (FdList[j].Fd == NOFD)
	    break;
    FdBottom = j;

    if (Fd >= 0) {
	FD_SET(Fd, &save_rfds);
	if (max_fds < Fd)
	    max_fds = Fd;
    }
    return Slot;
}

void UnRegisterRemote(uldat Slot) {
    /* not needed, we are going to quit anyway if this gets called */
}

void RemoteCouldntWrite(uldat Slot) {
    if (Slot == NOSLOT || Slot >= FdTop || LS.Fd == NOFD)
	return;
    if (LS.extern_couldntwrite == FALSE) {
	LS.extern_couldntwrite = TRUE;
	FdWQueued++;
    }
    FD_SET(LS.Fd, &save_wfds);
}

void RemoteCouldWrite(uldat Slot) {
    if (Slot == NOSLOT || Slot >= FdTop || LS.Fd == NOFD)
	return;
    if (LS.extern_couldntwrite == TRUE) {
	LS.extern_couldntwrite = FALSE;
	FdWQueued--;
    }
    FD_CLR(LS.Fd, &save_wfds);
}

static void RemoteEvent(int FdCount, fd_set *FdSet) {
    uldat Slot;
    int fd;
    for (Slot=0; Slot<FdTop && FdCount; Slot++) {
	if ((fd = LS.Fd) >= 0) {
	    if (FD_ISSET(fd, FdSet)) {
		FdCount--;
		if (LS.HandlerData)
		    (*(void (*)(int, void *))LS.HandlerIO) (fd, LS.HandlerData);
		else
		    (*(void (*)(int, uldat))LS.HandlerIO) (fd, Slot);
	    }
	}
    }
}


#ifdef CONF__MODULES

#include <dlfcn.h>

static fn_module _FnModule = {
    module_magic, (uldat)sizeof(module), (uldat)1,
	(void *)NoOp, /* CreateModule */
	(void *)NoOp, /* CopyModule   */
	(void *)NoOp, /* InsertModule */
	(void *)NoOp, /* RemoveModule */
	(void *)NoOp, /* DeleteModule */
	(void *)NoOp, /* DlOpen	      */
	(void *)NoOp  /* DlClose      */
};

static struct module _Module = {
    module_magic,
	&_FnModule,
};

static module *DlLoadAny(uldat len, byte *name) {
    module *Module = &_Module;
    byte (*init_dl)(module *);
    byte *_name;
    
    if ((Module->Name = CloneStrL(name, len)) &&
	(_name = AllocMem(len + strlen(twindir) + 1))) {
	
	sprintf(_name, "%s%s", twindir, Module->Name);

	if ((Module->Handle = dlopen(_name, RTLD_NOW|RTLD_GLOBAL)) &&
	    /*
	     * Module MUST have a InitModule function, as it needs to set
	     * Module->Private to its xxx_InitHW() startup code.
	     */
	    (init_dl = dlsym(Module->Handle, "InitModule")) &&
	    init_dl(Module))

	    return Module;
	else
	    ErrStr = dlerror();
    } else
	ErrStr = "Out of memory!";
    return (module *)0;
}

static byte module_InitHW(void) {
    byte *name, *tmp;
    byte *(*InitD)(void);
    byte *arg = HW->Name;
    uldat len = HW->NameLen;
    module *Module;

    if (!arg || len <= 4)
	return FALSE;
    
    arg += 4; len -= 4; /* skip "-hw=" */
    
    name = memchr(arg, '@', len);
    tmp = memchr(arg, ',', len);
    if (tmp && (!name || tmp < name))
	name = tmp;
    if (name)
	len = name - arg;
    
    if ((name = AllocMem(len + 10))) {
	sprintf(name, "HW/hw_%.*s.so", (int)len, arg);
			
	Module = DlLoadAny(len+9, name);
	
	FreeMem(name);
	    
	if (Module) {
	    fprintf(stderr, "twdisplay: starting display driver module `HW/hw_%.*s.so'...\n", (int)len, arg);
	    
	    if ((InitD = Module->Private) && InitD()) {
		fprintf(stderr, "twdisplay: ...module `HW/hw_%.*s.so' successfully started.\n", (int)len, arg);
		HW->Module = Module; Module->Used++;
		return TRUE;
	    }
	    /*Delete(Module);*/
	}
    } else
	ErrStr = "Out of memory!";

    if (Module) {
	fprintf(stderr, "twdisplay: ...module `HW/hw_%.*s.so' failed to start.\n", (int)len, arg);
    } else
	fprintf(stderr, "twdisplay: unable to load display driver module `HW/hw_%.*s.so' :\n"
		"      %s\n", (int)len, arg, ErrStr);
    
    return FALSE;
}

#endif /* CONF__MODULES */

static display_hw *CreateDisplayHW(uldat len, byte *name);
static byte InitDisplayHW(display_hw *);
static void QuitDisplayHW(display_hw *);

static fn_display_hw _FnDisplayHW = {
    display_hw_magic, (uldat)sizeof(display_hw), (uldat)1,
	(void *)NoOp, /* CreateDisplayHW */
	(void *)NoOp, /* CopyDisplayHW   */
	(void *)NoOp, /* InsertDisplayHW */
	(void *)NoOp, /* RemoveDisplayHW */
	(void *)NoOp, /* DeleteDisplayHW */
	InitDisplayHW,
	QuitDisplayHW
};

static display_hw _HW = {
    display_hw_magic,
	&_FnDisplayHW,
};

#if defined(CONF_HW_X11) || defined(CONF_HW_TWIN) || defined(CONF_HW_DISPLAY) || defined(CONF_HW_TTY) || defined(CONF_HW_GGI)
static byte check4(byte *s, byte *arg) {
    if (arg && strncmp(s, arg, strlen(s))) {
	fprintf(stderr, "twdisplay: `-hw=%s' given, skipping `-hw=%s' display driver.\n",
		arg, s);
	return FALSE;
    } else if (arg)
	fprintf(stderr, "twdisplay: trying given `-hw=%s' display driver.\n", s);
    else
	fprintf(stderr, "twdisplay: autoprobing `-hw=%s' display driver.\n", s);
    return TRUE;
}

static void fix4(byte *s, display_hw *D_HW) {
    uldat len;
    if (!D_HW->NameLen) {
	if (D_HW->Name)
	    FreeMem(D_HW->Name), D_HW->Name = NULL;
	len = strlen(s) + 4;
	if ((D_HW->Name = AllocMem(len + 1))) {
	    sprintf(D_HW->Name, "-hw=%s", s);
	    D_HW->NameLen = len;
	}
    }
}
#endif /* defined(CONF_HW_X11) || defined(CONF_HW_TWIN) || defined(CONF_HW_DISPLAY) || defined(CONF_HW_TTY) || defined(CONF_HW_GGI) */

static void warn_NoHW(uldat len, char *arg, uldat tried) {
#ifdef CONF__MODULES
    if (!tried && !arg)
	    fputs("twdisplay: no display driver compiled into twdisplay.\n"
		  "      please run as `twdisplay [-twin@<TWDISPLAY>] -hw=<display>'\n", stderr);
    else
#endif
    {
	fputs("twdisplay: All display drivers failed", stderr);
	if (arg)
	    fprintf(stderr, " for `-hw=%.*s\'", (int)len, arg);
	else
	    putc('.', stderr);
	putc('\n', stderr);
    }
}

static void UpdateFlagsHW(void) {
    if (!HW->Quitted) {
	NeedOldVideo = HW->FlagsHW & FlHWNeedOldVideo;
	ExpensiveFlushVideo = HW->FlagsHW & FlHWExpensiveFlushVideo;
	CanDragArea = !!HW->CanDragArea;
    }
}

/*
 * InitDisplayHW runs HW specific InitXXX() functions, starting from best setup
 * and falling back in case some of them fails.
 */
static byte InitDisplayHW(display_hw *D_HW) {
    byte *arg = D_HW->Name;
    uldat tried = 0;
    byte success;

    SaveHW;
    SetHW(D_HW);

    D_HW->DisplayIsCTTY = D_HW->NeedHW = D_HW->FlagsHW = FALSE;
    
    if (arg && !strncmp(arg, "-hw=", 4))
	arg += 4;
    else
	arg = NULL;

    success =
#ifdef CONF_HW_X11
	(check4("X", arg) && (tried++, X11_InitHW()) && (fix4("X", D_HW), TRUE)) ||
#endif
#ifdef CONF_HW_TWIN
	(check4("twin", arg) && (tried++, TW_InitHW()) && (fix4("twin", D_HW), TRUE)) ||
#endif

#if 0 /* cannot use `-hw=display' inside twdisplay! */
# ifdef CONF_HW_DISPLAY
	(check4("display", arg) && (tried++, display_InitHW()) && (fix4("display", D_HW), TRUE)) ||
# endif
#endif
	
#ifdef CONF_HW_TTY
	(check4("tty", arg) && (tried++, tty_InitHW()) && (fix4("tty", D_HW), TRUE)) ||
#endif
#ifdef CONF_HW_GGI
	(check4("ggi", arg) && (tried++, GGI_InitHW()) && (fix4("ggi", D_HW), TRUE)) ||
#endif
#ifdef CONF__MODULES
	module_InitHW() ||
#endif
	(warn_NoHW(arg ? D_HW->NameLen - 4 : 0, arg, tried), FALSE);

    if (success) {
	D_HW->Quitted = FALSE;
	if (!DisplayHWCTTY && D_HW->DisplayIsCTTY)
	    DisplayHWCTTY = D_HW;
	UpdateFlagsHW();
    }
    
    fflush(stderr);
    RestoreHW;

    return success;
}

static void QuitDisplayHW(display_hw *D_HW) {
    if (D_HW && D_HW->QuitHW)
	HW = D_HW, D_HW->QuitHW();
}


static display_hw *CreateDisplayHW(uldat NameLen, byte *Name) {
    byte *newName = NULL;
    
    if (Name && (newName = CloneStrL(Name, NameLen))) {	
	HW = &_HW;
	HW->NameLen = NameLen;
	HW->Name = newName;
	HW->Module = NULL;
	HW->Quitted = TRUE;
	HW->AttachSlot = NOSLOT;
	/*
	 * ->Quitted will be set to FALSE only
	 * immediately before trying DisplayHW->InitHW()
	 */
	return HW;
    }
    if (newName)
	FreeMem(newName);
    return (display_hw *)0;
}

static display_hw *AttachDisplayHW(uldat len, byte *arg, uldat slot) {
    if ((len && len <= 4) || CmpMem("-hw=", arg, Min2(len,4))) {
	fprintf(stderr, "twdisplay: specified `%.*s\' is not `-hw=<display>\'\n",
		(int)len, arg);
	fflush(stderr);
	return (display_hw *)0;
    }
    
    if (CreateDisplayHW(len, arg)) {
	HW->AttachSlot = slot;
	if (InitDisplayHW(HW))
	    return HW;
    }
    return (display_hw *)0;
}


INLINE void OptimizeChangedVideo(void) {
    uldat _start, start, _end, end;
    int i;
    
    for (i=0; i<ScreenHeight*2; i++) {
	start = (uldat)ChangedVideo[i>>1][!(i&1)][0];
	    
	if (start != (uldat)-1) {
	    
	    start += (i>>1) * ScreenWidth;
	    _start = start;

	    _end = end = (uldat)ChangedVideo[i>>1][!(i&1)][1] + (i>>1) * ScreenWidth;
		
	    while (start <= end && Video[start] == OldVideo[start])
		start++;
	    while (start <= end && Video[end] == OldVideo[end])
		end--;

	    if (start > end) {
		if (i&1) {
		    /*
		     * this is the first area, to make it empty
		     * copy the second on this.
		     */
		    if (ChangedVideo[i>>1][1][0] != -1) {
			ChangedVideo[i>>1][0][0] = ChangedVideo[i>>1][1][0];
			ChangedVideo[i>>1][0][1] = ChangedVideo[i>>1][1][1];
			ChangedVideo[i>>1][1][0] = -1;
		    } else
			ChangedVideo[i>>1][0][0] = -1;
		} else
		    ChangedVideo[i>>1][1][0] = -1;
		continue;
	    } else if (start > _start || end < _end) {
		ChangedVideo[i>>1][!(i&1)][0] += start - _start;
		ChangedVideo[i>>1][!(i&1)][1] -= _end - end;
	    }
	}
    }
}

INLINE void SyncOldVideo(void) {
    uldat start, len;
    int i;

    if (ChangedVideoFlag) {
	for (i=0; i<ScreenHeight*2; i++) {
	    start = ChangedVideo[i>>1][i&1][0];
	    
	    if (start != -1) {
		len = ChangedVideo[i>>1][i&1][1] + 1 - start;
		start += (i>>1)*ScreenWidth;
		
		ChangedVideo[i>>1][i&1][0] = -1;
	    
		CopyMem(Video + start, OldVideo + start, len * sizeof(hwattr));
	    }
	}
    }
}

void FlushHW(void) {
    
    if (!ValidVideo)
	return;
    
    if (HW->RedrawVideo) {
	DirtyVideo(HW->RedrawLeft, HW->RedrawUp, HW->RedrawRight, HW->RedrawDown);
	ValidOldVideo = FALSE;
    } else if (NeedOldVideo && ValidOldVideo)
	OptimizeChangedVideo();

    HW->FlushVideo();
    
    HW->RedrawVideo = FALSE;
    
    if (HW->NeedHW & NEEDFlushHW)
	HW->FlushHW();

    if (NeedHW & NEEDFlushStdout)
	fflush(stdout), NeedHW &= ~NEEDFlushStdout;
    
    SyncOldVideo();

    ChangedVideoFlag = FALSE;
    ValidOldVideo = TRUE;
}


void ResizeDisplayPrefer(display_hw *D_HW) {
    SaveHW;
    SetHW(D_HW);
    D_HW->DetectSize(&TryScreenWidth, &TryScreenHeight);
    NeedHW |= NEEDResizeDisplay;
    RestoreHW;
}

static byte ReAllocVideo(udat Width, udat Height) {
    byte change = ScreenWidth != Width || ScreenHeight != Height;
    
    if (!NeedOldVideo && OldVideo) {
	FreeMem(OldVideo);
	OldVideo = NULL;
    } else if ((NeedOldVideo && !OldVideo) || change) {
	if (!(OldVideo = (hwattr *)ReAllocMem(OldVideo, Width*Height*sizeof(hwattr))) && Width && Height) {
	    OutOfMemory();
	    Quit(1);
	}
	ValidOldVideo = FALSE;
    }
    
    if (!Video || change) {
	ScreenWidth = Width;
	ScreenHeight = Height;
	
	if ((!(Video = (hwattr *)ReAllocMem(Video, ScreenWidth*ScreenHeight*sizeof(hwattr))) ||
	     !(ChangedVideo = (dat (*)[2][2])ReAllocMem(ChangedVideo, ScreenHeight*sizeof(dat)*4))) &&
	     ScreenWidth && ScreenHeight) {
	    
	    OutOfMemory();
	    Quit(1);
	}
	ValidVideo = FALSE;
	WriteMem(ChangedVideo, 0xff, ScreenHeight*sizeof(dat)*4);
    
    }
    return change;
}


/*
 * return TRUE if ScreenWidth or ScreenHeight were changed
 */
static byte ResizeDisplay(void) {
    byte change = FALSE;
    tmsg Tmsg;
    
    if (!TryScreenWidth || !TryScreenHeight)
	HW->DetectSize(&TryScreenWidth, &TryScreenHeight);
    
    HW->CheckResize(&TryScreenWidth, &TryScreenHeight);
    HW->Resize(TryScreenWidth, TryScreenHeight);

    change = ReAllocVideo(TryScreenWidth, TryScreenHeight);
    
    NeedHW &= ~NEEDResizeDisplay;

    TryScreenWidth = TryScreenHeight = 0;
    
    if (change && (Tmsg = TwCreateMsg(TW_MSG_DISPLAY, sizeof(struct tevent_display)))) {
	Tmsg->Event.EventDisplay.Code = TW_DPY_Resize;
	Tmsg->Event.EventDisplay.Len  = 0;
	Tmsg->Event.EventDisplay.X    = ScreenWidth;
	Tmsg->Event.EventDisplay.Y    = ScreenHeight;
	TwBlindSendMsg(THelper, Tmsg);
    }
    return change;
}

byte AllHWCanDragAreaNow(dat Left, dat Up, dat Rgt, dat Dwn, dat DstLeft, dat DstUp) {
    return (CanDragArea && HW->CanDragArea &&
	    HW->CanDragArea(Left, Up, Rgt, Dwn, DstLeft, DstUp));
}

void DragAreaHW(dat Left, dat Up, dat Rgt, dat Dwn, dat DstLeft, dat DstUp) {
    HW->DragArea(Left, Up, Rgt, Dwn, DstLeft, DstUp);
}

    
void SetPaletteHW(udat N, udat R, udat G, udat B) {
    if (N <= MAXCOL) {
	palette c = {R, G, B};
	if (CmpMem(&Palette[N], &c, sizeof(palette))) {
	    Palette[N] = c;
	    HW->SetPalette(N, R, G, B);
	}
    }
}

void ResetPaletteHW(void) {
    HW->ResetPalette();
}

static void HandleMsg(tmsg Msg) {
    tevent_display EventD;
    
    switch (Msg->Type) {
      case TW_MSG_SELECTION:
	/* should never happen */
	fputs("\ntwdisplay: HandleMsg(): unexpected Selection Message from twin!\n", stderr);
	fflush(stderr);
	break;
      case TW_MSG_SELECTIONREQUEST:
#if 0
	fprintf(stderr, "twdisplay: Selection Request from 0x%08x, owner is underlying HW\n", Msg->Event.EventSelectionRequest.Requestor);
	fflush(stderr);
#endif
	/* request selection from underlying HW */
	HW->HWSelectionRequest((void *)Msg->Event.EventSelectionRequest.Requestor,
			       Msg->Event.EventSelectionRequest.ReqPrivate);
	break;
      case TW_MSG_SELECTIONNOTIFY:
#if 0
	fprintf(stderr, "twdisplay: Selection Notify to underlying HW\n");
	fflush(stderr);
#endif
	/* notify selection to underlying HW */
	HW->HWSelectionNotify(Msg->Event.EventSelectionNotify.ReqPrivate,
			      Msg->Event.EventSelectionNotify.Magic,
			      Msg->Event.EventSelectionNotify.MIME,
			      Msg->Event.EventSelectionNotify.Len,
			      Msg->Event.EventSelectionNotify.Data);
	break;
      case TW_MSG_DISPLAY:
	EventD = &Msg->Event.EventDisplay;
	switch (EventD->Code) {
	  case TW_DPY_DrawHWAttr:
	    if (EventD->Len /= sizeof(hwattr)) {
		if (EventD->X + EventD->Len > ScreenWidth || EventD->Y >= ScreenHeight) {
		    /*
		     * in a perfect world this should not happen, 
		     * but with our asyncronous display size negotiation,
		     * it actually can.
		     */

		    if (EventD->Y < ScreenHeight && EventD->X < ScreenWidth)
			EventD->Len = ScreenWidth - EventD->X;
		    else
			break;
		}
		DirtyVideo(EventD->X, EventD->Y, EventD->X + EventD->Len - 1, EventD->Y);
		CopyMem(EventD->Data, &Video[EventD->X + EventD->Y * ScreenWidth], EventD->Len * sizeof(hwattr));
	    }
	    break;
	  case TW_DPY_FlushHW:
	    ValidVideo = TRUE;
	    FlushHW();
	    break;
	  case TW_DPY_SetCursorType:
	    if (EventD->Len == sizeof(uldat))
		SetCursorType(*(uldat *)EventD->Data);
	    break;
	  case TW_DPY_MoveToXY:
	    MoveToXY(EventD->X, EventD->Y);
	    break;
	  case TW_DPY_Resize:
	    /*
	     * twin told us the new display size.
	     * don't detect/check anything, just apply it
	     * (if it is meaningful and different from current)
	     */
	    if (EventD->X != ScreenWidth || EventD->Y != ScreenHeight) {
		HW->Resize(EventD->X, EventD->Y);
		ReAllocVideo(EventD->X, EventD->Y);
	    }
	    break;
	  case TW_DPY_SelectionExport:
	    NeedHW |= NEEDSelectionExport;
	    break;
	  case TW_DPY_DragArea:
#define c ((udat *)EventD->Data)
	    if (EventD->Len == 4*sizeof(udat))
		DragArea(EventD->X, EventD->Y, c[0], c[1], c[2], c[3]);
#undef c
	    break;
	  case TW_DPY_Beep:
	    HW->Beep();
	    break;
	  case TW_DPY_Configure:
	    HW->Configure(EventD->X, (udat)EventD->Y == MAXUDAT, EventD->Y);
	    break;
	  case TW_DPY_SetPalette:
#define c ((udat *)EventD->Data)
	    if (EventD->Len == 3*sizeof(udat))
		HW->SetPalette(EventD->X, c[0], c[1], c[2]);
#undef c
	    break;
	  case TW_DPY_ResetPalette:
	    HW->ResetPalette();
	    break;
	  case TW_DPY_Helper:
	    THelper = *(uldat *)EventD->Data;
	    break;
	  case TW_DPY_Quit:
	    QuitDisplayHW(HW);
	    Quit(0);
	    break;
	  default:
	    break;
	}
    }
}

static void SocketIO(void) {
    tmsg Msg;
    while ((Msg = TwReadMsg(FALSE)))
	HandleMsg(Msg);
}

void SelectionExport(void) {
    HW->HWSelectionExport();
    NeedHW &= ~NEEDSelectionExport;
}

/* HW back-end function: get selection owner */
obj *TwinSelectionGetOwner(void) {
    return (void *)TwGetOwnerSelection();
}

/* HW back-end function: set selection owner */
void TwinSelectionSetOwner(obj *Owner, time_t Time, frac_t Frac) {
    tmsg Msg;

    if ((Msg=TwCreateMsg(TW_MSG_SELECTIONCLEAR, sizeof(tevent_common)))) {
	TwBlindSendMsg(THelper, Msg);
    }
}

/* HW back-end function: notify selection */
void TwinSelectionNotify(obj *Requestor, uldat ReqPrivate, uldat Magic, byte MIME[MAX_MIMELEN],
			    uldat Len, byte *Data) {
    if (!MIME)
	MIME = nullMIME;
#if 0
    fprintf(stderr, "twdisplay: Selection Notify to 0x%08x\n", (uldat)Requestor);
    fflush(stderr);
#endif
    TwNotifySelection((uldat)Requestor, ReqPrivate, Magic, MIME, Len, Data);
}

#if 0
void TwinInternalSelectionNotify(obj *Requestor, uldat ReqPrivate) {
    TwInternalSelectionNotify((uldat)Requestor, ReqPrivate);
}
#endif

/* HW back-end function: request selection */
void TwinSelectionRequest(obj *Requestor, uldat ReqPrivate, obj *Owner) {
#if 0
    fprintf(stderr, "twdisplay: Selection Request from 0x%08x, Owner is 0x%08x\n", (uldat)Requestor, (uldat)Owner);
    fflush(stderr);
#endif
    TwRequestSelection((uldat)Owner, ReqPrivate);
}

static byte StdAddEventMouse(udat CodeMsg, udat Code, dat MouseX, dat MouseY) {
    tevent_mouse Event;
    tmsg Msg;

    if (HW->FlagsHW & FlHWNoInput)
	return TRUE;

    if ((Msg=TwCreateMsg(CodeMsg, sizeof(event_mouse)))) {
	Event = &Msg->Event.EventMouse;

	Event->Code = Code;
	Event->ShiftFlags = (udat)0;
	Event->X = MouseX;
	Event->Y = MouseY;
	
	TwBlindSendMsg(THelper, Msg);
	return TRUE;
    }
    return FALSE;
}

byte MouseEventCommon(dat x, dat y, dat dx, dat dy, udat Buttons) {
    dat prev_x, prev_y;
    udat OldButtons;
    mouse_state *OldState;
    udat result;
    byte ret = TRUE;
     
    OldState=&HW->MouseState;
    OldButtons=OldState->keys;
    prev_x = OldState->x;
    prev_y = OldState->y;

    x = Max2(x, 0); x = Min2(x, ScreenWidth - 1);
    OldState->delta_x = x == 0 ? Min2(dx, 0) : x == ScreenWidth - 1 ? Max2(dx, 0) : 0;

    y = Max2(y, 0); y = Min2(y, ScreenHeight - 1);
    OldState->delta_y = y == 0 ? Min2(dy, 0) : y == ScreenHeight - 1 ? Max2(dy, 0) : 0;
	
    if (x != prev_x || y != prev_y)
	HW->FlagsHW |= FlHWChangedMouseFlag;

    OldState->x = x;
    OldState->y = y;
    
    OldState->keys=Buttons;
    
    if (Buttons != OldButtons || (OldButtons && (x != prev_x || y != prev_y))) {
	
	if (OldButtons && (x != prev_x || y != prev_y)) {
	    if (!StdAddEventMouse(TW_MSG_WINDOW_MOUSE, DRAG_MOUSE | OldButtons, x, y))
		ret = FALSE;
	}
	if ((Buttons & HOLD_LEFT) != (OldButtons & HOLD_LEFT)) {
	    result = (Buttons & HOLD_LEFT ? DOWN_LEFT : RELEASE_LEFT) | (OldButtons &= ~HOLD_LEFT);
	    OldButtons |= Buttons & HOLD_LEFT;
	    if (!StdAddEventMouse(TW_MSG_WINDOW_MOUSE, result, x, y))
		ret = FALSE;
	}
	if ((Buttons & HOLD_MIDDLE) != (OldButtons & HOLD_MIDDLE)) {
	    result = (Buttons & HOLD_MIDDLE ? DOWN_MIDDLE : RELEASE_MIDDLE) | (OldButtons &= ~HOLD_MIDDLE);
	    OldButtons |= Buttons & HOLD_MIDDLE;
	    if (!StdAddEventMouse(TW_MSG_WINDOW_MOUSE, result, x, y))
		ret = FALSE;
	}
	if ((Buttons & HOLD_RIGHT) != (OldButtons & HOLD_RIGHT)) {
	    result = (Buttons & HOLD_RIGHT ? DOWN_RIGHT : RELEASE_RIGHT) | (OldButtons &= ~HOLD_RIGHT);
	    OldButtons |= Buttons & HOLD_RIGHT;
	    if (!StdAddEventMouse(TW_MSG_WINDOW_MOUSE, result, x, y))
		ret = FALSE;
	}
    }
    return ret;
}

byte KeyboardEventCommon(udat Code, udat Len, byte *Seq) {
    tevent_keyboard Event;
    tmsg Msg;

    if (HW->FlagsHW & FlHWNoInput)
	return TRUE;

    if ((Msg=TwCreateMsg(TW_MSG_WINDOW_KEY, Len + sizeof(event_keyboard)))) {
	Event = &Msg->Event.EventKeyboard;
	    
	Event->Code = Code;
	Event->ShiftFlags = (udat)0;
	Event->SeqLen = Len;
	CopyMem(Seq, Event->AsciiSeq, Len);
	Event->AsciiSeq[Len] = '\0'; /* terminate string with \0 */
	
	TwBlindSendMsg(THelper, Msg);
	return TRUE;
    }
    return FALSE;
}

static void MainLoop(int Fd) {
    struct timeval sel_timeout, *this_timeout;
    fd_set read_fds, write_fds, *pwrite_fds;
    int num_fds;
    
    for (;;) {
	if (NeedHW & NEEDResizeDisplay)
	    ResizeDisplay();
	
	if (NeedHW & NEEDSelectionExport)
	    SelectionExport();

	/*
	 * don't FlushHW() unconditionalluy here,
	 * as it gets called syncronously
	 * with the main twin from HandleEvent()...
	 * just update what is needed.
	 */
	if (HW->RedrawVideo)
	    FlushHW();
	else {
	    HW->UpdateMouseAndCursor();

	    if (HW->NeedHW & NEEDFlushHW)
		HW->FlushHW();
	    if (NeedHW & NEEDFlushStdout)
		fflush(stdout), NeedHW &= ~NEEDFlushStdout;
	}
	
	read_fds = save_rfds;
	
	if (!FdWQueued)
	    pwrite_fds = NULL;
	else {
	    write_fds = save_wfds;
	    pwrite_fds = &write_fds;
	}
	
	if (!TwFlush())
	    break;
	
	if (TwPeekMsg()) {
	    sel_timeout.tv_sec = sel_timeout.tv_usec = 0;
	    this_timeout = &sel_timeout;
	} else
	    this_timeout = NULL;

	if (NeedHW & NEEDPanicHW)
	    break;

	num_fds = OverrideSelect(max_fds+1, &read_fds, pwrite_fds, NULL, this_timeout);

	if (num_fds < 0 && errno != EINTR)
	    /* ach, problem. */
	    break;

	if ((num_fds > 0 && FD_ISSET(Fd, &read_fds)) || TwPeekMsg()) {
	    /*
	     * messages can arrive during Tw* function calls,
	     * so the FD_ISSET() test alone does not suffice.
	     */
	    FD_CLR(Fd, &read_fds);
	    num_fds--;
	    SocketIO();
	}
	if (num_fds > 0)
	    RemoteEvent(num_fds, &read_fds);
    }

    if (NeedHW & NEEDPanicHW)
	Quit(0);
    
    if (num_fds < 0 && errno != EINTR) {
	QuitDisplayHW(HW);
	fprintf(stderr, "twdisplay: select(): %s\n", strerror(errno));
	exit(1);
    }
    if (TwInPanic()) {
    	QuitDisplayHW(HW);
	fprintf(stderr, "%s: libTw error: %s\n", MYname, TwStrError(TwErrno));
	exit(1);
    }
}

udat GetDisplayWidth(void) {
    return savedScreenWidth;
}
udat GetDisplayHeight(void) {
    return savedScreenHeight;
}


static void Usage(void) {
    fputs("Usage: twdisplay [OPTION [...]]\n"
	  "Currently known options: \n"
	  " -h, -help               display this help and exit\n"
	  " -V, -version            output version information and exit\n"
	  " -v                      verbose output (default)\n"
	  " -s                      silent; don't report messages from twin server\n"
	  " -twin@<TWDISPLAY>       specify server to contact instead of $TWDISPLAY\n"
	  " -hw=<display>[,options] start the given display\n"
	  "Currently known display methods: \n"
	  "\tX[@<XDISPLAY>]\n"
	  "\ttwin[@<TWDISPLAY>]\n"
	  "\ttty[@<tty device>]\n"
	  "\tggi[@<ggi display>]\n", stdout);
}

static void TryUsage(char *opt) {
    if (opt)
	fprintf(stdout, "twdisplay: unknown option `%s'\n", opt);
    fputs("           try `twdisplay -help' for usage summary.\n", stdout);
}

static void ShowVersion(void) {
    fputs("twdisplay " TWIN_VERSION_STR "\n", stdout);
}

int main(int argc, char *argv[]) {
    byte redirect = 1;
    byte *dpy = NULL, *arg = NULL, *tty = ttyname(0);
    byte ret = 0, ourtty = 0;
    byte *s, *buff;
    uldat chunk;
    int Fd;
    
    MYname = argv[0];
    
    while (*++argv) {
	if (!strcmp(*argv, "-V") || !strcmp(*argv, "-version")) {
	    ShowVersion();
	    return 0;
	} else if (!strcmp(*argv, "-h") || !strcmp(*argv, "-help")) {
	    Usage();
	    return 0;
	} else if (!strcmp(*argv, "-v"))
	    redirect = 1;
	else if (!strcmp(*argv, "-s"))
	    redirect = 0;
	else if (!strncmp(*argv, "-twin@", 6))
	    dpy = *argv + 6;
	else if (!strncmp(*argv, "-hw=", 4)) {
	    if (!strncmp(*argv+4, "display", 7)) {
		fprintf(stderr, "%s: argument `-hw=display' is for internal use only.\n", MYname);
		TryUsage(NULL);
		return 1;
	    }
	    if (!strncmp(*argv+4, "tty", 3)) {
		buff = *argv + 7;
		s = strchr(buff, ',');
		if (s) *s = '\0';
		
		if (!*buff || (*buff == '@' && (!buff[1] || !strcmp(buff+1, tty))))
		    /* attach twin to our tty */
		    ourtty = 1;
		/*
		 * using server tty makes no sense for twdisplay
		 */
		if (s) *s = ',';
		else s = "";
		
		if (ourtty) {
		    buff = getenv("TERM");
		    if (!buff) buff = "";
		    
		    arg = malloc(strlen(tty) + 9 + strlen(s) + (buff ? 6 + strlen(buff) : 0));
		    
		    sprintf(arg, "-hw=tty%s%s%s", (buff ? (byte *)",TERM=" : buff), buff, s);
		} else
		    arg = *argv;
	    } else if ((*argv)[4])
		arg = *argv;
	    else {
		TryUsage(*argv);
		return 1;
	    }
	} else {
	    TryUsage(*argv);
	    return 1;
	}
    }
    
    if (!arg) {
	Usage();
	return 1;
    }

    TWDisplay = origTWDisplay = CloneStr(dpy ? dpy : (byte *)getenv("TWDISPLAY"));
    origTERM = CloneStr(getenv("TERM"));

    InitSignals();
    InitTtysave();
    
#ifdef CONF__ALLOC
    TwConfigMalloc(AllocMem, ReAllocMem, FreeMem);
#endif

    if (TwOpen(TWDisplay)) do {
	byte *buf;

	if (RegisterRemote(Fd = TwConnectionFd(), (void *)0, (void *)NoOp) == NOSLOT) {
	    TwClose();
	    OutOfMemory();
	    return 1;
	}
	    
	if (!(TMsgPort = TwCreateMsgPort(9, "twdisplay", (uldat)0, (udat)0, (byte)0)))
	    break;

	savedScreenWidth = TryScreenWidth = TwGetDisplayWidth();
	savedScreenHeight = TryScreenHeight = TwGetDisplayHeight();

	if (!(HW = AttachDisplayHW(strlen(arg), arg, NOSLOT))) {
	    TwClose();
	    return 1;
	}
	if (!(buf = AllocMem(HW->NameLen + 80))) {
	    TwClose();
	    OutOfMemory();
	    return 1;
	}
	
	sprintf(buf, "-hw=display@(%.*s),x=%d,y=%d%s%s%s", (int)HW->NameLen, HW->Name,
		(int)HW->X, (int)HW->Y, HW->CanResize ? ",resize" : "",
		/* CanDragArea */ TRUE ? ",drag" : "", ExpensiveFlushVideo ? ",slow" : "");
	
	TwAttachHW(strlen(buf), buf, redirect);
	TwFlush();
	
	if (redirect)
	    fprintf(stderr, "reported messages ...\n");
	
	for (;;) {
	    buff = TwAttachGetReply(&chunk);
	    if (buff <= (byte *)2) {
		ret = (byte)(size_t)buff;
		break;
	    } else if (buff == (byte *)-1)
		/* libTw panic */
		break;

	    fprintf(stderr, "%.*s", (int)chunk, buff);
	}
	fflush(stderr);
	
	if (TwInPanic())
	    break;

	if (ourtty) {
	    fputs("\033[2J", stdout);
	    fflush(stdout);
	}

	
	TwAttachConfirm();
	/*
	 * twin waits this before grabbing the display...
	 * so we can fflush(stdout) to show all messages
	 * *BEFORE* twin draws on (eventually) the same tty
	 */

	ResizeDisplay();
	
	if (redirect && !ourtty) {
	    if (ret)
		fprintf(stderr, "... ok, twin successfully attached.\n");
	    else
		fprintf(stderr, "... ach, twin failed to attach.\n");
	    fflush(stderr);
	}
	if (ret == 2)
	    /*
	     * twin told us to stay and sit on the display
	     * until it is quitted.
	     */
	    MainLoop(Fd);
	else
	    fprintf(stderr, "%s: twin said we can quit... strange!\n", MYname);
	return !ret;
    } while (0);
    
    fprintf(stderr, "%s: libTw error: %s\n", MYname, TwStrError(TwErrno));
    return 1;
}

void Quit(int status) {
    QuitDisplayHW(HW);
    if (status < 0)
	return; /* give control back to signal handler */
    exit(status);
}