/****************************************************************/
/*                                                              */
/*                          version.h                           */
/*                                                              */
/*                  Common version information                  */
/*                                                              */
/*                      Copyright (c) 1997                      */
/*                      Pasquale J. Villani                     */
/*                      All Rights Reserved                     */
/*                                                              */
/* This file is part of DOS-C.                                  */
/*                                                              */
/* DOS-C is free software; you can redistribute it and/or       */
/* modify it under the terms of the GNU General Public License  */
/* as published by the Free Software Foundation; either version */
/* 2, or (at your option) any later version.                    */
/*                                                              */
/* DOS-C is distributed in the hope that it will be useful, but */
/* WITHOUT ANY WARRANTY; without even the implied warranty of   */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See    */
/* the GNU General Public License for more details.             */
/*                                                              */
/* You should have received a copy of the GNU General Public    */
/* License along with DOS-C; see the file COPYING.  If not,     */
/* write to the Free Software Foundation, 675 Mass Ave,         */
/* Cambridge, MA 02139, USA.                                    */
/****************************************************************/

#ifdef MAIN
#ifdef VERSION_STRINGS
static BYTE *date_hRcsId =
    "$Id$";
#endif
#endif

/* This Kernel is at a min Dos Ver. 5.00 or 7.10 with FAT32 support */
#ifdef WITHFAT32
#define MAJOR_RELEASE   7
#define MINOR_RELEASE   10
#else
#define MAJOR_RELEASE   5
#define MINOR_RELEASE   00
#endif

#define REV_NUMBER      0
#define OEM_ID          0xfd    /* FreeDos version                      */

#define REVISION_MAJOR  1
#define REVISION_MINOR  1
#define REVISION_SEQ    37
#define BUILD           "2037"
#define SUB_BUILD	"w-UNSTABLE"  /* only use w=WorkInProgress for UNSTABLE branch */
#define KERNEL_VERSION_STRING "1.1.37w" /*#REVISION_MAJOR "." #REVISION_MINOR "." #REVISION_SEQ */
#define KERNEL_BUILD_STRING "2037w-UNSTABLE"   /*#BUILD SUB_BUILD */
