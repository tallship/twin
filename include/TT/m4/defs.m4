




dnl This file is `m4/defs.m4' : m4 macros to autogenerate defsm4.h
dnl Tell the user about this.
/* This file was automatically generated from `m4/defs.m4', do not edit! */


/*
 *  defsm4.h  --  public header for libTT types declarations
 *
 *  Copyright (C) 2001 by Massimiliano Ghilardi
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 */

#ifndef _TT_DEFSM4_H
#define _TT_DEFSM4_H

include(`m4/TTclasses.m4h')

#define TT_LIST(el) \
TTlist()

#define TT_NLIST(el) \
TTnlist()

/* typedefs for objects structures */
define(`el',`
typedef struct s_$1 *	$1;')
TTlist()
undefine(`el')

/* typedefs for methods structures (runtime types) */
typedef struct s_ttfn *	ttfn;

/* useful types */
typedef void (*ttcallback_fn)(ttuint nargs, ttany *args);
typedef void (*ttvisible_repaint_fn)(ttvisible,ttshort,ttshort,ttshort,ttshort);

/* global methods structures */
define(`el',`
extern ttfn	TTFN_$1;')
TTlist()
undefine(`el')
extern ttfn	TTFN_ttfn;

/* #defines for objects hierarchy */
define(`extends',`
`#'define TTsuper_$2	$1')
define(`field')
define(`el',`TTdef_$1($1)')
TTlist()
undefine(`el')
define(`field')
define(`extends')

define(`args_decl', `regexp(`$1', `^.*(.*).*(\(.*\))', `\1')')
dnl define(`args_decl', `NSHIFT(5, $@)')

/* `public' and `exported' methods */
define(`m4TTdeclare',`define(`m4TT$2_$3',`TT$2_$3')
$5 TT$2_$3(ifelse($4, 0, `void', `args_decl(`$1')'));`'dnl
')

define(`el',`

/* $1 `public' and `exported' methods */dnl
TTFNdef_$1($1)')

define(`extends')
define(`public',`m4TTdeclare($@)')
define(`exported',`public($@)')
TTlist()
define(`exported')
define(`public')
define(`extends')
undefine(`el')

dnl 
dnl define(`m4TTbasesuper',`ifdef(`m4TT$2_$3',`m4TT$2_$3',`m4TTbasesuper(,`$2',m4super_$3)')')
dnl 
dnl /* methods inherited by derivate classes */
dnl /* set #defines pointing to the most primitive class implementing the method */
dnl define(`extends')
dnl define(`public',`
dnl `#'define TT$2_$3	m4TTbasesuper(,$2,$3)')
dnl define(`exported',`public($@)')
dnl define(`mysuper',`ifelse($1,ttobj,`',`mysuper(m4super_$1,$2)')`'TTFNdef_$1($2)')
dnl define(`el',`
dnl 
dnl /* methods inherited by $1 */dnl
dnl mysuper(m4super_$1,$1)')
dnl 
dnl TTnlist()
dnl undefine(`el')
dnl undefine(`mysuper')
dnl define(`exported')
dnl define(`public')
dnl define(`extends')
dnl 

#endif /* _TT_DEFSM4_H */
