/****************************************************************/
/*                                                              */
/*                           dosfns.c                           */
/*                                                              */
/*                         DOS functions                        */
/*                                                              */
/*                      Copyright (c) 1995                      */
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

#include "portab.h"
#include "debug.h"

#ifdef VERSION_STRINGS
static BYTE *dosfnsRcsId =
    "$Id$";
#endif

#include "globals.h"

/* /// Added for SHARE.  - Ron Cemer */

BYTE share_installed = 0;

        /* DOS calls this to see if it's okay to open the file.
           Returns a file_table entry number to use (>= 0) if okay
           to open.  Otherwise returns < 0 and may generate a critical
           error.  If < 0 is returned, it is the negated error return
           code, so DOS simply negates this value and returns it in
           AX. */
extern int ASMPASCAL
           share_open_check(char * filename,            /* pointer to fully qualified filename */
                            unsigned short pspseg,      /* psp segment address of owner process */
                            int openmode,       /* 0=read-only, 1=write-only, 2=read-write */
                            int sharemode);     /* SHARE_COMPAT, etc... */

        /* DOS calls this to record the fact that it has successfully
           closed a file, or the fact that the open for this file failed. */
extern void ASMPASCAL
            share_close_file(int fileno);       /* file_table entry number */

        /* DOS calls this to determine whether it can access (read or
           write) a specific section of a file.  We call it internally
           from lock_unlock (only when locking) to see if any portion
           of the requested region is already locked.  If pspseg is zero,
           then it matches any pspseg in the lock table.  Otherwise, only
           locks which DO NOT belong to pspseg will be considered.
           Returns zero if okay to access or lock (no portion of the
           region is already locked).  Otherwise returns non-zero and
           generates a critical error (if allowcriter is non-zero).
           If non-zero is returned, it is the negated return value for
           the DOS call. */
extern int ASMPASCAL
            share_access_check(unsigned short pspseg,    /* psp segment address of owner process */
                              int fileno,       /* file_table entry number */
                              unsigned long ofs,        /* offset into file */
                              unsigned long len,        /* length (in bytes) of region to access */
                              int allowcriter); /* allow a critical error to be generated */

        /* DOS calls this to lock or unlock a specific section of a file.
           Returns zero if successfully locked or unlocked.  Otherwise
           returns non-zero.
           If the return value is non-zero, it is the negated error
           return code for the DOS 0x5c call. */
extern int ASMPASCAL
            share_lock_unlock(unsigned short pspseg,     /* psp segment address of owner process */
                             int fileno,        /* file_table entry number */
                             unsigned long ofs, /* offset into file */
                             unsigned long len, /* length (in bytes) of region to lock or unlock */
                             int unlock);       /* one to unlock; zero to lock */

/* /// End of additions for SHARE.  - Ron Cemer */

STATIC int remote_lock_unlock(sft FAR *sftp,    /* SFT for file */
                             unsigned long ofs, /* offset into file */
                             unsigned long len, /* length (in bytes) of region to lock or unlock */
                             int unlock);       /* one to unlock; zero to lock */

/* get current directory structure for drive
   return NULL if the CDS is not valid or the
   drive is not within range, on input drv is 
   0 for default or 1=A,2=B,...
*/
struct cds FAR *get_cds1(unsigned drv)
{
  if (drv-- == 0) /* get default drive or convert to 0 = A:, 1 = B:, ... */
    drv = default_drive;
  return get_cds(drv);
}

struct cds FAR *get_cds(unsigned drive)
{
  struct cds FAR *cdsp;
  unsigned flags;

  if (drive >= lastdrive)
    return NULL;
  cdsp = &CDSp[drive];
  flags = cdsp->cdsFlags;
  /* Entry is disabled or JOINed drives are accessable by the path only */
  if ((flags & CDSVALID) == 0 ||
      (flags & CDSJOINED) != 0 ||
      (flags & CDSNETWDRV) == 0 && cdsp->cdsDpb == NULL)
    return NULL;
  return cdsp;
}

#ifdef WITHFAT32
struct dpb FAR * GetDriveDPB(UBYTE drive, COUNT * rc)
{
  struct dpb FAR *dpb;
  struct cds FAR *cdsp;
  
  cdsp = get_cds(drive == 0 ? default_drive : drive - 1);
  
  if (cdsp == NULL)
  {
    *rc = DE_INVLDDRV;
    return 0;
  }
  dpb = cdsp->cdsDpb;
  if (dpb == 0 || cdsp->cdsFlags & CDSNETWDRV)
  {
    *rc = DE_INVLDDRV;
    return 0;
  }

  *rc = SUCCESS;
  return dpb;
}
#endif

/* Construct dir-style filename for ASCIIZ 8.3 name */
STATIC VOID DosGetFile(BYTE * lpszPath, BYTE FAR * lpszDosFileName)
{
  char fcbname[FNAME_SIZE + FEXT_SIZE];

  ParseDosName(lpszPath, fcbname, FALSE);
  fmemcpy(lpszDosFileName, fcbname, FNAME_SIZE + FEXT_SIZE);
}

int idx_to_sft_(int SftIndex)
{
  /*called from below and int2f/ax=1216*/
  sfttbl FAR *sp;

  lpCurSft = (sft FAR *) - 1;
  if (SftIndex < 0)
    return -1;

  /* Get the SFT block that contains the SFT      */
  for (sp = sfthead; sp != (sfttbl FAR *) - 1; sp = sp->sftt_next)
  {
    if (SftIndex < sp->sftt_count)
    {
      /* finally, point to the right entry            */
      lpCurSft = (sft FAR *) & (sp->sftt_table[SftIndex]);
      return SftIndex;
    }
    SftIndex -= sp->sftt_count;
  }

  /* If not found, return an error                */
  return -1;
}

sft FAR * idx_to_sft(int SftIndex)
{
  /* called internally only */
  SftIndex = idx_to_sft_(SftIndex);
  /* if not opened, the SFT is useless            */
  if (SftIndex == -1 || lpCurSft->sft_count == 0)
    return (sft FAR *) - 1;
  return lpCurSft;
}
      
int get_sft_idx(unsigned hndl)
{
  psp FAR *p = MK_FP(cu_psp, 0);
  int idx;

  if (hndl >= p->ps_maxfiles)
    return DE_INVLDHNDL;

  idx = p->ps_filetab[hndl];
  return idx == 0xff ? DE_INVLDHNDL : idx;
}

sft FAR *get_sft(UCOUNT hndl)
{
  /* Get the SFT block that contains the SFT      */
  return idx_to_sft(get_sft_idx(hndl));
}

long DosRWSft(int sft_idx, size_t n, void FAR * bp, int mode)
{
  /* Get the SFT block that contains the SFT      */
  sft FAR *s = idx_to_sft(sft_idx);

  if (FP_OFF(s) == (size_t) - 1)
  {
    return DE_INVLDHNDL;
  }
  /* If for read and write-only or for write and read-only then exit */
  if((mode == XFR_READ && (s->sft_mode & O_WRONLY)) ||
     (mode == XFR_WRITE && (s->sft_mode & O_ACCMODE) == O_RDONLY))
  {
    return DE_ACCESS;
  }
  if (mode == XFR_FORCE_WRITE)
    mode = XFR_WRITE;
    
/*
 *   Do remote first or return error.
 *   must have been opened from remote.
 */
  if (s->sft_flags & SFT_FSHARED)
  {
    long XferCount;
    VOID FAR *save_dta;

    save_dta = dta;
    lpCurSft = s;
    current_filepos = s->sft_posit;     /* needed for MSCDEX */
    dta = bp;
    XferCount = remote_rw(mode == XFR_READ ? REM_READ : REM_WRITE, s, n);
    dta = save_dta;
    return XferCount;
  }

  /* Do a device transfer if device                   */
  if (s->sft_flags & SFT_FDEVICE)
  {
    struct dhdr FAR *dev = s->sft_dev;

    /* Now handle raw and cooked modes      */
    if (s->sft_flags & SFT_FBINARY)
    {
      long rc = BinaryCharIO(&dev, n, bp,
                             mode == XFR_READ ? C_INPUT : C_OUTPUT);
      if (mode == XFR_WRITE && rc > 0 && (s->sft_flags & SFT_FCONOUT))
      {
        size_t cnt = (size_t)rc;
        const char FAR *p = bp;
        while (cnt--)
          update_scr_pos(*p++, 1);
      }
      return rc;
    }

    /* cooked mode */
    if (mode==XFR_READ)
    {
      long rc;

      /* Test for eof and exit                */
      /* immediately if it is                 */
      if (!(s->sft_flags & SFT_FEOF) || (s->sft_flags & SFT_FNUL))
        return 0;

      if (s->sft_flags & SFT_FCONIN)
        rc = read_line_handle(sft_idx, n, bp);
      else
        rc = cooked_read(&dev, n, bp);
      if (*(char *)bp == CTL_Z)
        s->sft_flags &= ~SFT_FEOF;
      return rc;
    }
    else
    {
      /* reset EOF state (set to no EOF)      */
      s->sft_flags |= SFT_FEOF;

      /* if null just report full transfer    */
      if (s->sft_flags & SFT_FNUL)
        return n;
      else
        return cooked_write(&dev, n, bp);
    }
  }

  /* a block transfer                           */
  /* /// Added for SHARE - Ron Cemer */
  if (IsShareInstalled() && (s->sft_shroff >= 0))
  {
    int rc = share_access_check(cu_psp, s->sft_shroff, s->sft_posit,
                                 (unsigned long)n, 1);
    if (rc != SUCCESS)
      return rc;
  }
  /* /// End of additions for SHARE - Ron Cemer */
  {
    long XferCount = rwblock(s->sft_status, bp, n, mode);
    if (XferCount < 0)
      return XferCount;
    if (mode == XFR_WRITE)
      s->sft_size = dos_getfsize(s->sft_status);
    s->sft_posit += XferCount;
    return XferCount;
  }
}

int SftSeek(int sft_idx, LONG new_pos, unsigned mode)
{
  return _SftSeek(idx_to_sft(sft_idx), new_pos, mode);
}

int _SftSeek(sft FAR *s, LONG new_pos, unsigned mode)
{
  if (FP_OFF(s) == (size_t) -1)
    return DE_INVLDHNDL;
        
  /* Test for invalid mode                        */
  if (mode > SEEK_END) /* 2 */
    return DE_INVLDFUNC;

  lpCurSft = s;

  if (s->sft_flags & SFT_FSHARED)
  {
    if (mode == SEEK_CUR)
    {
      new_pos += s->sft_posit;
    }
    /* seek from end of file */
    else if (mode == SEEK_END)
    {
/*
 *  RB list has it as Note:
 *  this function is called by the DOS 3.1+ kernel, but only when seeking
 *  from the end of a file opened with sharing modes set in such a manner
 *  that another process is able to change the size of the file while it
 *  is already open
 *  Tested this with Shsucdx ver 0.06 and 1.0. Both now work.
 *  Lredir via mfs.c from DosEMU works when writing appended files.
 *  Mfs.c looks for these mode bits set, so here is my best guess.;^)
 */
      if (s->sft_mode & (O_DENYREAD | O_DENYNONE))
        new_pos = remote_lseek(s, new_pos);
      else
        new_pos += s->sft_size;
    }
  }
  /* Do special return for character devices      */
  else if (s->sft_flags & SFT_FDEVICE)
  {
    new_pos = 0;
  }
  else
  {
    if ((new_pos = dos_lseek(s->sft_status, new_pos, mode)) < 0)
      return (int)new_pos;
  }

  s->sft_posit = new_pos;
  return SUCCESS;
}

STATIC long get_free_hndl(void)
{
  psp FAR *p = MK_FP(cu_psp, 0);
  UBYTE FAR *q = p->ps_filetab;
  UBYTE FAR *r = fmemchr(q, 0xff, p->ps_maxfiles);
  return FP_OFF(r) == 0 ? DE_TOOMANY : r - q;
}

STATIC sft FAR *get_free_sft(COUNT * sft_idx)
{
  COUNT sys_idx = 0;
  sfttbl FAR *sp;

  /* Get the SFT block that contains the SFT      */
  for (sp = sfthead; sp != (sfttbl FAR *) - 1; sp = sp->sftt_next)
  {
    REG COUNT i = sp->sftt_count;
    sft FAR *sfti = sp->sftt_table;

    for (; --i >= 0; sys_idx++, sfti++)
    {
      if (sfti->sft_count == 0)
      {
        *sft_idx = sys_idx;

        /* MS NET uses this on open/creat TE */
        {
          extern WORD ASM current_sft_idx;
          current_sft_idx = sys_idx;
        }

        return sfti;
      }
    }
  }
  /* If not found, return an error                */
  return (sft FAR *) - 1;
}

const char FAR *get_root(const char FAR * fname)
{
  /* find the end                                 */
  register unsigned length = fstrlen(fname);
  char c;

  /* now back up to first path seperator or start */
  fname += length;
  while (length)
  {
    length--;
    c = *--fname;
    if (c == '/' || c == '\\' || c == ':') {
      fname++;
      break;
    }
  }
  return fname;
}

/* initialize SFT fields (for open/creat) for character devices */
STATIC int DeviceOpenSft(struct dhdr FAR *dhp, sft FAR *sftp)
{
  int i;

  sftp->sft_shroff = -1;      /* /// Added for SHARE - Ron Cemer */
  sftp->sft_count += 1;
  sftp->sft_flags =
    (dhp->dh_attr & ~(SFT_MASK | SFT_FSHARED)) | SFT_FDEVICE | SFT_FEOF;
  fmemcpy(sftp->sft_name, dhp->dh_name, FNAME_SIZE);

  /* pad with spaces */
  for (i = FNAME_SIZE + FEXT_SIZE - 1; sftp->sft_name[i] == '\0'; i--)
    sftp->sft_name[i] = ' ';
  /* and uppercase */
  DosUpFMem(sftp->sft_name, FNAME_SIZE + FEXT_SIZE);

  sftp->sft_dev = dhp;
  sftp->sft_date = dos_getdate();
  sftp->sft_time = dos_gettime();
  sftp->sft_attrib = D_DEVICE;

  if (sftp->sft_dev->dh_attr & SFT_FOCRM)
  {
    /* if Open/Close/RM bit in driver's attribute is set
     * then issue an Open request to the driver
     */
    struct dhdr FAR *dev = sftp->sft_dev;
    if (BinaryCharIO(&dev, 0, MK_FP(0x0000, 0x0000), C_OPEN) != SUCCESS)
      return DE_ACCESS;
  }
  return SUCCESS;
}

/*
extended open codes
0000 0000 always fail
0000 0001 open O_OPEN
0000 0010 replace O_TRUNC

0001 0000 create new file O_CREAT
0001 0001 create if not exists, open if exists O_CREAT | O_OPEN
0001 0010 create O_CREAT | O_TRUNC

bits for flags (bits 11-8 are internal FreeDOS bits only)
15 O_FCB  called from FCB open
14 O_SYNC commit for each write (not implemented yet)
13 O_NOCRIT do not invoke int23 (not implemented yet)
12 O_LARGEFILE allow files >= 2gb but < 4gb (not implemented yet)
11 O_LEGACY not called from int21/ah=6c: find right fn for redirector
10 O_CREAT if file does not exist, create it
9 O_TRUNC if file exists, truncate and open it \ not both 
8 O_OPEN  if file exists, open it              /
7 O_NOINHERIT do not inherit handle on exec
6 \ 
5  - sharing modes
4 / 
3 reserved 
2 bits 2,1,0 = 100: RDONLY and do not modify file's last access time
                    (not implemented yet)
1 \ 0=O_RDONLY, 1=O_WRONLY,
0 / 2=O_RDWR, 3=O_EXECCASE (preserve case for redirector EXEC,
                            (not implemented yet))
*/

long DosOpenSft(char FAR * fname, unsigned flags, unsigned attrib)
{
  COUNT sft_idx;
  sft FAR *sftp;
  struct dhdr FAR *dhp;
  long result;

  result = truename(fname, PriPathName, CDS_MODE_CHECK_DEV_PATH);
  if (result < SUCCESS)
  {
    DFnsDbgPrintf(("DosOpenSft: truename failed\n"));
    return result;
  }
  else DFnsDbgPrintf(("DosOpenSft: truename succeeded\n"));

  /* now get a free system file table entry       */
  if ((sftp = get_free_sft(&sft_idx)) == (sft FAR *) - 1)
  {
    DFnsDbgPrintf(("DosOpenSft: file table full\n"));
    return DE_TOOMANY;
  }
  else DFnsDbgPrintf(("DosOpenSft: obtained sft entry\n"));

  fmemset(sftp, 0, sizeof(sft));

  sftp->sft_psp = cu_psp;
  sftp->sft_mode = flags & 0xf0ff;
  OpenMode = (BYTE) flags;

  sftp->sft_shroff = -1;        /* /// Added for SHARE - Ron Cemer */
  sftp->sft_attrib = attrib = attrib | D_ARCHIVE;

  DFnsDbgPrintf(("DosOpenSft: mode=%02X, flags=%02X, attrib=%02X\n", \
               sftp->sft_mode, OpenMode, attrib));

  if (result & IS_NETWORK)
  {
    int status;
    unsigned cmd;
    DFnsDbgPrintf(("DosOpenSft: IS_NETWORK\n"));
    if ((flags & (O_TRUNC | O_CREAT)) == O_CREAT)
      attrib |= 0x100;

    lpCurSft = sftp;
    cmd = REM_CREATE;
    if (!(flags & O_LEGACY))
    {
      extern UWORD ASM ext_open_mode, ASM ext_open_attrib, ASM ext_open_action;
      ext_open_mode = flags & 0x70ff;
      ext_open_attrib = attrib & 0xff;
      ext_open_action = ((flags & 0x0300) >> 8) | ((flags & O_CREAT) >> 6);
      cmd = REM_EXTOC;
    }
    else if (!(flags & O_CREAT))
    {
      cmd = REM_OPEN;
      attrib = (BYTE)flags;
    }
    status = (int)network_redirector_mx(cmd, sftp, (void *)attrib);
    if (status >= SUCCESS)
    {
      if (sftp->sft_count == 0)
        sftp->sft_count++;
      return sft_idx | ((long)status << 16);
    }
    return status;
  }

  /* check for a device   */
  if ((result & IS_DEVICE) && (dhp = IsDevice(fname)) != NULL)
  {
    int rc = DeviceOpenSft(dhp, sftp);
    /* check the status code returned by the
     * driver when we tried to open it
     */
    DFnsDbgPrintf(("DosOpenSft: IS_DEVICE\n"));
    if (rc < SUCCESS)
      return rc;
    return sft_idx;
  }

/* /// Added for SHARE.  - Ron Cemer */
  if (IsShareInstalled())
  {
    DFnsDbgPrintf(("DosOpenSft: using SHARE\n"));
    if ((sftp->sft_shroff =
         share_open_check(PriPathName, cu_psp,
                          flags & 0x03, (flags >> 4) & 0x07)) < 0)
      return sftp->sft_shroff;
  }
  
/* /// End of additions for SHARE.  - Ron Cemer */

  /* NEVER EVER allow directories to be created */
  /* ... though FCB's are weird :) */
  if (!(flags & O_FCB) &&
      (attrib & ~(D_RDONLY | D_HIDDEN | D_SYSTEM | D_ARCHIVE | D_VOLID)))
  {
    DFnsDbgPrintf(("DosOpenSft: invalid request\n"));
    return DE_ACCESS;
  }
  
  result = dos_open(PriPathName, flags, attrib);
  DFnsDbgPrintf(("DosOpenSft: dos_open returned %02X\n", result));
  if (result >= 0)
  {
    int status = (int)(result >> 16);
    if (status == S_OPENED)
    {
      sftp->sft_attrib = dos_getfattr_fd((COUNT)result);
      /* Check permissions. -- JPP
         (do not allow to open volume labels/directories) */
      if (sftp->sft_attrib & (D_DIR | D_VOLID))
      {
        DFnsDbgPrintf(("DosOpenSft: closing, invalid attributes DIR|VOLID\n"));
        dos_close((COUNT)result);
        return DE_ACCESS;
      }
      sftp->sft_size = dos_getfsize((COUNT)result);
    }
    sftp->sft_status = (COUNT)result;
    sftp->sft_flags = PriPathName[0] - 'A';
    sftp->sft_count += 1;
    DosGetFile(PriPathName, sftp->sft_name);
    dos_getftime(sftp->sft_status,
                 (date FAR *) & sftp->sft_date,
                 (time FAR *) & sftp->sft_time);
    DFnsDbgPrintf(("DosOpenSft: returning %lu (decimal)\n", (sft_idx | ((long)status << 16))));
    return sft_idx | ((long)status << 16);
  }
  else
  {
/* /// Added for SHARE *** CURLY BRACES ADDED ALSO!!! ***.  - Ron Cemer */
    if (IsShareInstalled())
    {
      share_close_file(sftp->sft_shroff);
      sftp->sft_shroff = -1;
    }
/* /// End of additions for SHARE.  - Ron Cemer */
    return result;
  }    
}

long DosOpen(char FAR * fname, unsigned mode, unsigned attrib)
{
  long result;
  unsigned hndl;
  
  /* test if mode is in range                     */
  if ((mode & ~O_VALIDMASK) != 0)
    return DE_INVLDACC;

  /* get a free handle  */
  if ((result = get_free_hndl()) < 0)
    return result;
  hndl = (unsigned)result;

  result = DosOpenSft(fname, mode, attrib);
  if (result < SUCCESS)
    return result;

  ((psp FAR *)MK_FP(cu_psp, 0))->ps_filetab[hndl] = (UBYTE)result;
  return hndl | (result & 0xffff0000l);
}

COUNT CloneHandle(unsigned hndl)
{
  /* now get the system file table entry                          */
  sft FAR *sftp = get_sft(hndl);

  if (sftp == (sft FAR *) -1 || (sftp->sft_mode & O_NOINHERIT))
    return DE_INVLDHNDL;
  
  /* now that we have the system file table entry, get the fnode  */
  /* index, and increment the count, so that we've effectively    */
  /* cloned the file.                                             */
  sftp->sft_count += 1;
  return SUCCESS;
}

long DosDup(unsigned Handle)
{
  long NewHandle;

  if ((NewHandle = get_free_hndl()) < 0)
    return NewHandle;

  if (DosForceDup(Handle, (unsigned)NewHandle) < 0)
    return DE_INVLDHNDL;
  else
    return NewHandle;
}

COUNT DosForceDup(unsigned OldHandle, unsigned NewHandle)
{
  psp FAR *p = MK_FP(cu_psp, 0);
  sft FAR *Sftp;

  /* Get the SFT block that contains the SFT                      */
  if ((Sftp = get_sft(OldHandle)) == (sft FAR *) - 1)
    return DE_INVLDHNDL;

  /* now close the new handle if it's open                        */
  if ((UBYTE) p->ps_filetab[NewHandle] != 0xff)
  {
    COUNT ret;

    if ((ret = DosClose(NewHandle)) != SUCCESS)
      return ret;
  }

  /* If everything looks ok, bump it up.                          */
  if ((Sftp->sft_flags & (SFT_FDEVICE | SFT_FSHARED))
      || (Sftp->sft_status >= 0))
  {
    p->ps_filetab[NewHandle] = p->ps_filetab[OldHandle];
    /* possible hazard: integer overflow ska*/
    Sftp->sft_count += 1;
    return SUCCESS;
  }
  else
    return DE_INVLDHNDL;
}

COUNT DosCloseSft(int sft_idx, BOOL commitonly)
{
  sft FAR *sftp = idx_to_sft(sft_idx);

  if (FP_OFF(sftp) == (size_t) - 1)
    return DE_INVLDHNDL;

  lpCurSft = sftp;
/*
   remote sub sft_count.
 */
  if (sftp->sft_flags & SFT_FSHARED)
  {
    /* printf("closing SFT %d = %p\n",sft_idx,sftp); */
    return network_redirector_fp(commitonly ? REM_FLUSH: REM_CLOSE, sftp);
  }

  /* now just drop the count if a device, else    */
  /* call file system handler                     */
  if (!commitonly)
    sftp->sft_count -= 1;

  if (sftp->sft_flags & SFT_FDEVICE)
  {
    if (sftp->sft_dev->dh_attr & SFT_FOCRM)
    {
      /* if Open/Close/RM bit in driver's attribute is set
       * then issue a Close request to the driver
       */
      struct dhdr FAR *dev = sftp->sft_dev;
      if (BinaryCharIO(&dev, 0, MK_FP(0x0000, 0x0000), C_CLOSE) != SUCCESS)
        return DE_ACCESS;
    }
    return SUCCESS;
  }
  if (commitonly || sftp->sft_count > 0)
    return dos_commit(sftp->sft_status);

/* /// Added for SHARE *** CURLY BRACES ADDED ALSO!!! ***.  - Ron Cemer */
  if (IsShareInstalled())
  {
    if (sftp->sft_shroff >= 0)
      share_close_file(sftp->sft_shroff);
    sftp->sft_shroff = -1;
  }
/* /// End of additions for SHARE.  - Ron Cemer */
  return dos_close(sftp->sft_status);
}

COUNT DosClose(COUNT hndl)
{
  psp FAR *p = MK_FP(cu_psp, 0);
  COUNT ret;

  /* Get the SFT block that contains the SFT      */
  ret = DosCloseSft(get_sft_idx(hndl), FALSE);
  if (ret != DE_INVLDHNDL && ret != DE_ACCESS)
    p->ps_filetab[hndl] = 0xff;
  return ret;
}

/* get disk free space (in terms of free clusters and cluster size)
   input: drive specifies which disk to obtain information about, 
          where 0=default, 1=A,2=B,...
          navc, bps, and nc are pointers to uninitialized variables
          used to hold the result, where
          navc is count of available [free] clusters
          bps is bytes per sector
          nc is total number of clusters
   returns the number of sectors per cluster or on error -1
 */
UWORD DosGetFree(UBYTE drive, UWORD * navc, UWORD * bps, UWORD * nc)
{
  /* navc==NULL means: called from FatGetDrvData, fcbfns.c */
  struct dpb FAR *dpbp;
  struct cds FAR *cdsp;
  UWORD spc;

  /* first check for valid drive				*/
  if ((cdsp = get_cds1(drive)) == NULL)
    return -1;

  if (cdsp->cdsFlags & CDSNETWDRV)
  {
    COUNT rg[4];
    if (remote_getfree(cdsp, rg) != SUCCESS)
      return -1;

    /* for int21/ah=1c:
       Undoc DOS says, its not supported for
       network drives. so it's probably OK */
    /* some programs such as RHIDE want it though and
       the redirector can provide all info
       - Bart, 2002 Apr 1 */

    *bps = rg[2];
    *nc = rg[1];
    if (navc)
      *navc = rg[3];
    return rg[0];
  }

  if ((dpbp = cdsp->cdsDpb) == NULL)
    return -1;

  if (navc == NULL)
  {
      /* hazard: no error checking! */
    flush_buffers(dpbp->dpb_unit);
    dpbp->dpb_flags = M_CHANGED;
  }
  if (media_check(dpbp) < 0)
    return -1;

  /* get the data available from dpb				*/
  spc = dpbp->dpb_clsmask + 1;
  *bps = dpbp->dpb_secsize;

#ifdef WITHFAT32
  if (ISFAT32(dpbp))
  {
    ULONG cluster_size, ntotal, nfree;

    cluster_size = (ULONG) dpbp->dpb_secsize << dpbp->dpb_shftcnt;
    ntotal = dpbp->dpb_xsize - 1;
    /* now tell fs to give us free cluster count		*/
    if (navc)
      nfree = dos_free(dpbp);

    /* we shift ntotal until it is equal to or below 0xfff6	*/
    while (ntotal > FAT_MAGIC16 && cluster_size < 0x8000)
    {
      cluster_size <<= 1;
      spc <<= 1;
      ntotal >>= 1;
      nfree >>= 1;
    }

    *nc = ntotal > FAT_MAGIC16 ? FAT_MAGIC16 : (UWORD) ntotal;
    if (navc)
      *navc = nfree > FAT_MAGIC16 ? FAT_MAGIC16 : (UWORD) nfree;
    return spc;
  }
#endif

  *nc = dpbp->dpb_size - 1;
  /* now tell fs to give us free cluster count			*/
  if (navc)
    *navc = (UWORD) dos_free(dpbp);
  if (spc > 64)
  {
    /* fake for 64k clusters do confuse some DOS programs, but let
       others work without overflowing */
    spc >>= 1;
    if (*nc > FAT_MAGIC16 / 2)
      *nc = FAT_MAGIC16;
    else
      *nc <<= 1;
    if (navc)
      if (*navc > FAT_MAGIC16 / 2)
        *navc = FAT_MAGIC16;
      else
        *navc <<= 1;
  }
  return spc;
}

#ifdef WITHFAT32
/* network names like \\SERVER\C aren't supported yet */
#define IS_SLASH(ch) (ch == '\\' || ch == '/')
COUNT DosGetExtFree(BYTE FAR * DriveString, struct xfreespace FAR * xfsp)
{
  struct dpb FAR *dpbp;
  struct cds FAR *cdsp;
  UCOUNT rg[4];

  if (IS_SLASH(DriveString[0]) || !IS_SLASH(DriveString[2])
      || DriveString[1] != ':')
    return DE_INVLDDRV;

  cdsp = get_cds(DosUpFChar(*DriveString) - 'A');

  if (cdsp == NULL)
    return DE_INVLDDRV;

  if (cdsp->cdsFlags & CDSNETWDRV)
  {
    if (remote_getfree(cdsp, rg) != SUCCESS)
      return DE_INVLDDRV;

    xfsp->xfs_clussize = rg[0];
    xfsp->xfs_totalclusters = rg[1];
    xfsp->xfs_secsize = rg[2];
    xfsp->xfs_freeclusters = rg[3];
  }
  else
  {
    dpbp = cdsp->cdsDpb;
    if (dpbp == NULL || media_check(dpbp) < 0)
      return DE_INVLDDRV;
    xfsp->xfs_secsize = dpbp->dpb_secsize;
    xfsp->xfs_totalclusters =
        (ISFAT32(dpbp) ? dpbp->dpb_xsize : dpbp->dpb_size);
    xfsp->xfs_freeclusters = dos_free(dpbp);
    xfsp->xfs_clussize = dpbp->dpb_clsmask + 1;
  }
  xfsp->xfs_totalunits = xfsp->xfs_totalclusters;
  xfsp->xfs_freeunits = xfsp->xfs_freeclusters;
  xfsp->xfs_totalsectors = xfsp->xfs_totalclusters * xfsp->xfs_clussize;
  xfsp->xfs_freesectors = xfsp->xfs_freeclusters * xfsp->xfs_clussize;
  xfsp->xfs_datasize = sizeof(struct xfreespace);

  fmemset(xfsp->xfs_reserved, 0, 8);

  return SUCCESS;
}
#endif

COUNT DosGetCuDir(UBYTE drive, BYTE FAR * s)
{
  BYTE *cp;
  struct cds FAR *cdsp;

  /* next - "log" in the drive            */
  /* first check for valid drive          */
  cdsp = get_cds(drive == 0 ? default_drive : drive - 1);
  if (cdsp == NULL)
    return DE_INVLDDRV;

  fmemcpy(&TempCDS, cdsp, sizeof(TempCDS));
  cp = TempCDS.cdsCurrentPath;
  /* ensure termination of fstrcpy */
  cp[MAX_CDSPATH - 1] = '\0';

  if ((TempCDS.cdsFlags & CDSNETWDRV) == 0)
  {
    /* dos_cd ensures that the path exists; if not, we
       need to change to the root directory */
    int result = dos_cd(cp);
    if (result == DE_PATHNOTFND)
      cp[TempCDS.cdsBackslashOffset + 1] =
        cdsp->cdsCurrentPath[TempCDS.cdsBackslashOffset + 1] = '\0';
    else if (result < SUCCESS)
      return result;
  }

  cp += TempCDS.cdsBackslashOffset;
  if (*cp == '\0')
    s[0] = '\0';
  else
    fstrcpy(s, cp + 1);

  return SUCCESS;
}

COUNT DosChangeDir(BYTE FAR * s)
{
  COUNT result;
  BYTE FAR *p;

  /* don't do wildcard CHDIR --TE */
  for (p = s; *p; p++)
    if (*p == '*' || *p == '?')
      return DE_PATHNOTFND;

  result = truename(s, PriPathName, CDS_MODE_CHECK_DEV_PATH);
  if (result < SUCCESS)
  {
    return result;
  }

  if ((FP_OFF(current_ldt) != 0xFFFF) &&
      (strlen(PriPathName) >= sizeof(current_ldt->cdsCurrentPath)))
    return DE_PATHNOTFND;

#if defined(CHDIR_DEBUG)
  printf("Remote Chdir: n='%Fs' p='%Fs\n", s, PriPathName);
#endif
  /* now get fs to change to new          */
  /* directory                            */
  result = (result & IS_NETWORK ? network_redirector(REM_CHDIR) :
            dos_cd(PriPathName));
#if defined(CHDIR_DEBUG)
  printf("status = %04x, new_path='%Fs'\n", result, cdsd->cdsCurrentPath);
#endif
  if (result != SUCCESS)
    return result;
/*
   Copy the path to the current directory
   structure.

        Some redirectors do not write back to the CDS.
        SHSUCdX needs this. jt
*/
  fstrcpy(current_ldt->cdsCurrentPath, PriPathName);
  if (FP_OFF(current_ldt) != 0xFFFF)
  {
     fstrcpy(current_ldt->cdsCurrentPath, PriPathName);
     if (PriPathName[7] == 0)
       current_ldt->cdsCurrentPath[8] = 0; /* Need two Zeros at the end */
  }
  return SUCCESS;
}

STATIC int pop_dmp(int rc, dmatch FAR * save_dta)
{
  dta = save_dta;
  if (rc == SUCCESS)
  {
    fmemcpy(save_dta, &sda_tmp_dm, 21 /*offsetof(save_dta->dm_attr_fnd)*/ );
    save_dta->dm_attr_fnd = SearchDir.dir_attrib;
    save_dta->dm_time = SearchDir.dir_time;
    save_dta->dm_date = SearchDir.dir_date;
    save_dta->dm_size = SearchDir.dir_size;
    ConvertName83ToNameSZ(save_dta->dm_name, SearchDir.dir_name);
  }
  return rc;
}

COUNT DosFindFirst(UCOUNT attr, const char FAR * name)
{
  dmatch FAR *save_dta = dta;
  int rc = truename(name, PriPathName,
                    CDS_MODE_CHECK_DEV_PATH | CDS_MODE_ALLOW_WILDCARDS);
  if (rc < SUCCESS)
    return rc;

  /* /// Added code here to do matching against device names.
     DOS findfirst will match exact device names if the
     filename portion (excluding the extension) contains
     a valid device name.
     Credits: some of this code was ripped off from truename()
     in newstuff.c.
     - Ron Cemer */

  SAttr = (BYTE) attr;

#if defined(FIND_DEBUG)
  printf("Remote Find: n='%Fs\n", PriPathName);
#endif

  dta = &sda_tmp_dm;
  memset(&sda_tmp_dm, 0, sizeof(dmatch)+sizeof(struct dirent));

  if (rc & IS_NETWORK)
    rc = network_redirector_fp(REM_FINDFIRST, current_ldt);
  else if (rc & IS_DEVICE)
  {
    const char *p;
    COUNT i;

    /* Found a matching device. Hence there cannot be wildcards. */
    SearchDir.dir_attrib = D_DEVICE;
    SearchDir.dir_time = dos_gettime();
    SearchDir.dir_date = dos_getdate();
    p = (const char *)get_root(PriPathName);
    for (i = 0; i < FNAME_SIZE && *p && *p != '.'; i++)
      SearchDir.dir_name[i] = *p++;
    for (; i < FNAME_SIZE + FEXT_SIZE; i++)
      SearchDir.dir_name[i] = ' ';
    rc = SUCCESS;
    /* /// End of additions.  - Ron Cemer ; heavily edited - Bart Oldeman */
  }
  else
    rc = dos_findfirst(attr, PriPathName);

  return pop_dmp(rc, save_dta);
}

COUNT DosFindNext(void)
{
  dmatch FAR *save_dta = dta;

  /* findnext will always fail on a device name or volume id */
  if (save_dta->dm_attr_fnd & (D_DEVICE | D_VOLID))
    return DE_NFILES;

/*
 *  The new version of SHSUCDX 1.0 looks at the dm_drive byte to
 *  test 40h. I used RamView to see location MSD 116:04be and
 *  FD f??:04be, the byte set with 0xc4 = Remote/Network drive 4.
 *  Ralf Brown docs for dos 4eh say bit 7 set == remote so what is
 *  bit 6 for? 
 *  SHSUCDX Mod info say "test redir not network bit".
 *  Just to confuse the rest, MSCDEX sets bit 5 too.
 *
 *  So, assume bit 6 is redirector and bit 7 is network.
 *  jt
 *  Bart: dm_drive can be the drive _letter_.
 *  but better just stay independent of it: we only use
 *  bit 7 to detect a network drive; the rest untouched.
 *  RBIL says that findnext can only return one error type anyway
 *  (12h, DE_NFILES)
 */
#if 0
  printf("findnext: %d\n", save_dta->dm_drive);
#endif
  fmemcpy(dta = &sda_tmp_dm, save_dta, 21 /*offsetof(save_dta->dm_attr_fnd)*/ );
  memset(&SearchDir, 0, sizeof(struct dirent));
  return pop_dmp(sda_tmp_dm.dm_drive & 0x80
			? network_redirector_fp(REM_FINDNEXT, &sda_tmp_dm)
			: dos_findnext(), save_dta);
}

COUNT DosGetFtime(COUNT hndl, date * dp, time * tp)
{
  sft FAR *s;
/*sfttbl FAR *sp;*/

  /* Get the SFT block that contains the SFT      */
  if (FP_OFF(s = get_sft(hndl)) == (size_t) - 1)
    return DE_INVLDHNDL;

  /* If SFT entry refers to a device, return the date and time of opening */
  if (s->sft_flags & (SFT_FDEVICE | SFT_FSHARED))
  {
    *dp = s->sft_date;
    *tp = s->sft_time;
    return SUCCESS;
  }

  /* call file system handler                     */
  return dos_getftime(s->sft_status, dp, tp);
}

COUNT DosSetFtimeSft(int sft_idx, date dp, time tp)
{
  /* Get the SFT block that contains the SFT      */
  sft FAR *s = idx_to_sft(sft_idx);

  if (FP_OFF(s) == (size_t) - 1)
    return DE_INVLDHNDL;

  /* If SFT entry refers to a device, do nothing */
  if (s->sft_flags & SFT_FDEVICE)
    return SUCCESS;

  s->sft_flags |= SFT_FDATE;
  s->sft_date = dp;
  s->sft_time = tp;

  if (s->sft_flags & SFT_FSHARED)
    return SUCCESS;

  /* call file system handler                     */
  return dos_setftime(s->sft_status, dp, tp);
}

COUNT DosGetFattr(BYTE FAR * name)
{
  COUNT result;

  result = truename(name, PriPathName, CDS_MODE_CHECK_DEV_PATH);
  if (result < SUCCESS)
    return result;
  
/* /// Added check for "d:\", which returns 0x10 (subdirectory) under DOS.
       - Ron Cemer */
           /* Theoretically: If the redirectory's qualify function
               doesn't return nonsense this check can be reduced to
               PriPathname[3] == 0, because local path names always
               have the three-byte string ?:\ and UNC path shouldn't
               validy consist of just two slashes.
               -- 2001/09/03 ska*/

  if (PriPathName[3] == '\0')
    return 0x10;

  if (result & IS_NETWORK)
    return network_redirector(REM_GETATTRZ);

  if (result & IS_DEVICE)
    return DE_FILENOTFND;

/* /// Use truename()'s result, which we already have in PriPathName.
       I copy it to tmp_name because PriPathName is global and seems
       to get trashed somewhere in transit.
       The reason for using truename()'s result is that dos_?etfattr()
       are very low-level functions and don't handle full path expansion
       or cleanup, such as converting "c:\a\b\.\c\.." to "C:\A\B".
       - Ron Cemer
*/
/*
          memcpy(SecPathName,PriPathName,sizeof(SecPathName));
          return dos_getfattr(SecPathName, attrp);
*/
    /* no longer true. dos_getfattr() is 
       A) intelligent (uses dos_open) anyway
       B) there are some problems with MAX_PARSE, i.e. if PATH ~= 64
       and TRUENAME adds a C:, which leeds to trouble. 

       the problem was discovered, when VC did something like

       fd = DosOpen(filename,...)
       jc can't_copy_dialog;

       attr = DosGetAttrib(filename);
       jc can't_copy_dialog;
       and suddenly, the filehandle stays open
       shit.
       tom
     */
  return dos_getfattr(PriPathName);
}

/* This function is almost identical to DosGetFattr().
   Maybe it is nice to join both functions.
       -- 2001/09/03 ska*/
COUNT DosSetFattr(BYTE FAR * name, UWORD attrp)
{
  COUNT result;

  result = truename(name, PriPathName, CDS_MODE_CHECK_DEV_PATH);
  if (result < SUCCESS)
    return result;

  if (result & IS_NETWORK)
    return remote_setfattr(attrp);

  if (result & IS_DEVICE)
    return DE_FILENOTFND;

/* /// Use truename()'s result, which we already have in PriPathName.
       I copy it to tmp_name because PriPathName is global and seems
       to get trashed somewhere in transit.
       - Ron Cemer
*/
/*
          memcpy(SecPathName,PriPathName,sizeof(SecPathName));
          return dos_setfattr(SecPathName, attrp);
          
          see DosGetAttr()
*/
  return dos_setfattr(PriPathName, attrp);
}

UBYTE DosSelectDrv(UBYTE drv)
{
  current_ldt = get_cds(drv);

  if (current_ldt != NULL)
    default_drive = drv;

  return lastdrive;
}

COUNT DosDelete(BYTE FAR * path, int attrib)
{
  COUNT result;

  result = truename(path, PriPathName, CDS_MODE_CHECK_DEV_PATH);
  if (result < SUCCESS)
    return result;

  if (result & IS_NETWORK)
    return network_redirector(REM_DELETE);

  if (result & IS_DEVICE)
    return DE_FILENOTFND;

  return dos_delete(PriPathName, attrib);
}

COUNT DosRenameTrue(BYTE * path1, BYTE * path2, int attrib)
{
  if (path1[0] != path2[0])
  {
    return DE_DEVICE; /* not same device */
  }
  if (FP_OFF(current_ldt) == 0xFFFF || (current_ldt->cdsFlags & CDSNETWDRV))
    return network_redirector(REM_RENAME);

  return dos_rename(path1, path2, attrib);
}

COUNT DosRename(BYTE FAR * path1, BYTE FAR * path2)
{
  COUNT result;

  result = truename(path2, SecPathName, CDS_MODE_CHECK_DEV_PATH);
  if (result < SUCCESS)
    return result;

  if ((result & (IS_NETWORK | IS_DEVICE)) == IS_DEVICE)
    return DE_FILENOTFND;

  result = truename(path1, PriPathName, CDS_MODE_CHECK_DEV_PATH);
  if (result < SUCCESS)
    return result;

  if ((result & (IS_NETWORK | IS_DEVICE)) == IS_DEVICE)
    return DE_FILENOTFND;

  return DosRenameTrue(PriPathName, SecPathName, D_ALL);
}

COUNT DosMkRmdir(const char FAR * dir, int action)
{
  COUNT result;

  result = truename(dir, PriPathName, CDS_MODE_CHECK_DEV_PATH);
  if (result < SUCCESS)
    return result;

  if (result & IS_NETWORK)
    return network_redirector(action == 0x39 ? REM_MKDIR : REM_RMDIR);

  if (result & IS_DEVICE)
    return DE_ACCESS;

  return (action == 0x39 ? dos_mkdir : dos_rmdir)(PriPathName);
}

/* /// Added for SHARE.  - Ron Cemer */

COUNT DosLockUnlock(COUNT hndl, LONG pos, LONG len, COUNT unlock)
{
  sft FAR *s;

  /* Get the SFT block that contains the SFT      */
  if (FP_OFF(s = get_sft(hndl)) == (size_t) - 1)
    return DE_INVLDHNDL;

  if (s->sft_flags & SFT_FSHARED)
    return remote_lock_unlock(s, pos, len, unlock);

  /* Invalid function unless SHARE is installed or remote. */
  if (!IsShareInstalled())
    return DE_INVLDFUNC;

  /* Lock violation if this SFT entry does not support locking. */
  if (s->sft_shroff < 0)
    return DE_LOCK;

  /* Let SHARE do the work. */
  return share_lock_unlock(cu_psp, s->sft_shroff, pos, len, unlock);
}

/* /// End of additions for SHARE.  - Ron Cemer */

/*
 * This seems to work well.
 */

/* check for a device
   returns device header if match, else returns NULL
   can only match character devices (as only they have names)
 */
struct dhdr FAR *IsDevice(const char FAR * fname)
{
  struct dhdr FAR *dhp;
  const char FAR *froot = get_root(fname);
  int i;

/* /// BUG!!! This is absolutely wrong.  A filename of "NUL.LST" must be
       treated EXACTLY the same as a filename of "NUL".  The existence or
       content of the extension is irrelevent in determining whether a
       filename refers to a device.
       - Ron Cemer
  // if we have an extension, can't be a device <--- WRONG.
  if (*froot != '.')
  {
*/

/*  BUGFIX: MSCD000<00> should be handled like MSCD000<20> TE 
    ie the 8 character device name may be padded with spaces ' ' or NULs '\0'

    Note: fname is assumed an ASCIIZ string (ie not padded, unknown length)
    but the name in the device header is assumed FNAME_SIZE and padded.  KJD
*/


  /* check for names that will never be devices to avoid checking all device headers.
     only the file name (not path nor extension) need be checked, "" == root or empty name
   */
  if ( (*froot == '\0') ||
       ((*froot=='.') && ((*(froot+1)=='\0') || (*(froot+2)=='\0' && *(froot+1)=='.')))
     )
  {
    return NULL;
  }

  /* cycle through all device headers checking for match */
  for (dhp = (struct dhdr FAR *)&nul_dev; dhp != (struct dhdr FAR *)-1;
       dhp = dhp->dh_next)
  {
    if (!(dhp->dh_attr & ATTR_CHAR))  /* if this is block device, skip */
      continue;

    for (i = 0; i < FNAME_SIZE; i++)
    {
      unsigned char c1 = (unsigned char)froot[i];
      /* ignore extensions and handle filenames shorter than FNAME_SIZE */
      if (c1 == '.' || c1 == '\0')
      {
        /* check if remainder of device name consists of spaces or nulls */
        for (; i < FNAME_SIZE; i++)
        {
          unsigned char c2 = dhp->dh_name[i];
          if (c2 != ' ' && c2 != '\0')
            break;
        }
        break;
      }
      if (DosUpFChar(c1) != DosUpFChar(dhp->dh_name[i]))
        break;
    }

    /* if found a match then return device header */
    if (i == FNAME_SIZE)
      return dhp;
  }

  return NULL;
}

/* /// Added for SHARE.  - Ron Cemer */

BOOL IsShareInstalled(void)
{
  extern unsigned char ASMPASCAL share_check(void);
  if (!share_installed && share_check() == 0xff)
    share_installed = TRUE;
  return share_installed;
}

/* /// End of additions for SHARE.  - Ron Cemer */

COUNT DosTruename(const char FAR *src, char FAR *dest)
{
  /* RBIL: The buffer has be unchanged, if the call fails.
     Therefore, the name is created in an internal buffer
     and copied into the user buffer only on success.
  */  
  COUNT rc = truename(src, PriPathName, CDS_MODE_ALLOW_WILDCARDS);
  if (rc >= SUCCESS)
    fstrcpy(dest, PriPathName);
  return rc;
}

STATIC int remote_lock_unlock(sft FAR *sftp,     /* SFT for file */
                              unsigned long ofs, /* offset into file */
                              unsigned long len, /* length (in bytes) of region to lock or unlock */
                              int unlock)
                                 /* one to unlock; zero to lock */
{
  struct
  {
    unsigned long ofs, len;
    int unlock;
  } param_block;
  param_block.ofs = ofs;
  param_block.len = len;
  param_block.unlock = unlock;
  return (int)network_redirector_mx(REM_LOCK, sftp, &param_block);
}
