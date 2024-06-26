/****************************************************************/
/*                                                              */
/*                          fatdir.c                            */
/*                            DOS-C                             */
/*                                                              */
/*                 FAT File System dir Functions                */
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
#include "globals.h"
#include "debug.h"

#ifdef VERSION_STRINGS
static BYTE *fatdirRcsId =
    "$Id$";
#endif

/* Description.
 *  Initialize a fnode so that it will point to the directory with 
 *  dirstart starting cluster; in case of passing dirstart == 0
 *  fnode will point to the start of a root directory */
VOID dir_init_fnode(f_node_ptr fnp, CLUSTER dirstart)
{
  /* reset the directory flags    */
  fnp->f_flags = F_DDIR;
  fnp->f_diroff = 0;
  fnp->f_offset = 0l;
  fnp->f_cluster_offset = 0;

  /* root directory */
#ifdef WITHFAT32
  if (dirstart == 0)
    if (ISFAT32(fnp->f_dpb))
      dirstart = fnp->f_dpb->dpb_xrootclst;
#endif
  fnp->f_cluster = fnp->f_dirstart = dirstart;
}

f_node_ptr dir_open(register const char *dirname)
{
  f_node_ptr fnp;
  int i;
  char fcbname[FNAME_SIZE + FEXT_SIZE];

  FDirDbgPrintf(("dir_open: trying to open %s\n", dirname));
  
  /* Allocate an fnode if possible - error return (0) if not.     */
  if ((fnp = get_f_node()) == (f_node_ptr) 0)
  {
    FDirDbgPrintf(("dir_open: failed to get f_node\n"));
    return (f_node_ptr) 0;
  }

  /* Force the fnode into read-write mode                         */
  fnp->f_mode = RDWR;

  /* determine what drive and dpb we are using...                 */
  fnp->f_dpb = get_dpb(dirname[0]-'A');
  /* Perform all directory common handling after all special      */
  /* handling has been performed.                                 */

  if (media_check(fnp->f_dpb) < 0)
  {
    FDirDbgPrintf(("dir_open: media_check failed\n"));
    release_f_node(fnp);
    return (f_node_ptr) 0;
  }

  /* Walk the directory tree to find the starting cluster         */
  /*                                                              */
  /* Start from the root directory (dirstart = 0)                 */

  /* The CDS's cdsStartCls may be used to shorten the search
     beginning at the CWD, see mapPath() and CDS.H in order
     to enable this behaviour there.
           -- 2001/09/04 ska*/

  dir_init_fnode(fnp, 0);

  dirname += 2;               /* Assume FAT style drive       */
  while(*dirname != '\0')
  {
    /* skip all path seperators                             */
    while (*dirname == '\\')
      ++dirname;
    /* don't continue if we're at the end                   */
    if (*dirname == '\0')
      break;

    /* Convert the name into an absolute name for           */
    /* comparison...                                        */

    memset(fcbname, ' ', FNAME_SIZE + FEXT_SIZE);

    for (i = 0; i < FNAME_SIZE + FEXT_SIZE; i++, dirname++)
    {
      char c = *dirname;
      if (c == '.')
        i = FNAME_SIZE - 1;
      else if (c != '\0' && c != '\\')
        fcbname[i] = c;
      else
        break;
    }

    /* Now search through the directory to  */
    /* find the entry...                    */
    i = FALSE;

    while (dir_read(fnp) == 1)
    {
      if (!(fnp->f_dir.dir_attrib & D_VOLID) &&
          fcbmatch(fcbname, fnp->f_dir.dir_name))
      {
        i = TRUE;
        break;
      }
      fnp->f_diroff++;
    }

    if (!i || !(fnp->f_dir.dir_attrib & D_DIR))
    {
      FDirDbgPrintf(("dir_open: no match found, returning failure\n"));
      release_f_node(fnp);
      return (f_node_ptr) 0;
    }
    else
    {
      /* make certain we've moved off */
      /* root                         */
      dir_init_fnode(fnp, getdstart(fnp->f_dpb, &fnp->f_dir));
    }
  }
  FDirDbgPrintf(("dir_open: returning, ok\n"));
  return fnp;
}

/* swap internal and external delete flags */
STATIC void swap_deleted(char *name)
{
  if (name[0] == DELETED || name[0] == EXT_DELETED)
    name[0] ^= EXT_DELETED - DELETED; /* 0xe0 */
}

STATIC struct buffer FAR *getblock_from_off(f_node_ptr fnp, unsigned secsize)
{
  /* Compute the block within the cluster and the */
  /* offset within the block.                     */
  unsigned sector;

  sector = (UBYTE)(fnp->f_offset / secsize) & fnp->f_dpb->dpb_clsmask;

  /* Get the block we need from cache             */
  return getblock(clus2phys(fnp->f_cluster, fnp->f_dpb) + sector,
                  fnp->f_dpb->dpb_unit);
}

/* Description.
 *  Read next consequitive directory entry, pointed by fnp.
 *  If some error occures the other critical
 *  fields aren't changed, except those used for caching.
 *  The fnp->f_diroff always corresponds to the directory entry
 *  which has been read.
 * Return value.
 *  1              - all OK, directory entry having been read is not empty.
 *  0              - Directory entry is empty.
 *  DE_SEEK        - Attempt to read beyound the end of the directory.
 *  DE_BLKINVLD    - Invalid block.
 * Note. Empty directory entries always resides at the end of the directory. */
COUNT dir_read(REG f_node_ptr fnp)
{
  struct buffer FAR *bp;
  REG UWORD secsize = fnp->f_dpb->dpb_secsize;

  /* can't have more than 65535 directory entries */
  if (fnp->f_diroff >= 65535U)
  {
      FDirDbgPrintf(("dir_read: exceed dir entry count\n"));
      return DE_SEEK;
  }

  /* Determine if we hit the end of the directory. If we have,    */
  /* bump the offset back to the end and exit. If not, fill the   */
  /* dirent portion of the fnode, clear the f_dmod bit and leave, */
  /* but only for root directories                                */

  if (fnp->f_dirstart == 0)
  {
    if (fnp->f_diroff >= fnp->f_dpb->dpb_dirents)
    {
      FDirDbgPrintf(("dir_read: end of dir\n"));
      return DE_SEEK;
    }

    bp = getblock(fnp->f_diroff / (secsize / DIRENT_SIZE)
                           + fnp->f_dpb->dpb_dirstrt, fnp->f_dpb->dpb_unit);
#ifdef DISPLAY_GETBLOCK
    printf("DIR (dir_read)\n");
#endif
  }
  else
  {
    /* Do a "seek" to the directory position        */
    fnp->f_offset = fnp->f_diroff * (ULONG)DIRENT_SIZE;

    /* Search through the FAT to find the block     */
    /* that this entry is in.                       */
#ifdef DISPLAY_GETBLOCK
    printf("dir_read: ");
#endif
    if (map_cluster(fnp, XFR_READ) != SUCCESS)
    {
      DebugPrintf(("dir_read: map_cluster failed\n"));
      return DE_SEEK;
    }

    bp = getblock_from_off(fnp, secsize);
#ifdef DISPLAY_GETBLOCK
    printf("DIR (dir_read)\n");
#endif
  }

  /* Now that we have the block for our entry, get the    */
  /* directory entry.                                     */
  if (bp == NULL)
  {
    FDirDbgPrintf(("dir_read: invalid block\n"));
    return DE_BLKINVLD;
  }

  bp->b_flag &= ~(BFR_DATA | BFR_FAT);
  bp->b_flag |= BFR_DIR | BFR_VALID;

  getdirent((BYTE FAR *) & bp->
            b_buffer[(fnp->f_diroff * DIRENT_SIZE) % fnp->f_dpb->dpb_secsize],
            &fnp->f_dir);

  swap_deleted(fnp->f_dir.dir_name);

  /* Update the fnode's directory info                    */
  fnp->f_flags &= ~F_DMOD;

  /* and for efficiency, stop when we hit the first       */
  /* unused entry.                                        */
  /* either returns 1 or 0                                */
  FDirDbgPrintf(("dir_read: dir_name is %11s\n", fnp->f_dir.dir_name));
  return (fnp->f_dir.dir_name[0] != '\0');
}

/* Description.
 *  Writes directory entry pointed by fnp to disk. In case of erroneous
 *  situation fnode is released.
 *  The caller should set
 *    1. F_DMOD flag if original directory entry was modified.
 * Return value.
 *  TRUE  - all OK.
 *  FALSE - error occured (fnode is released).
 */
#ifndef IPL
BOOL dir_write(REG f_node_ptr fnp)
{
  struct buffer FAR *bp;
  REG UWORD secsize = fnp->f_dpb->dpb_secsize;

  if (!(fnp->f_flags & F_DDIR))
    return FALSE;

  /* Update the entry if it was modified by a write or create...  */
  if (fnp->f_flags & F_DMOD)
  {
    /* Root is a consecutive set of blocks, so handling is  */
    /* simple.                                              */
    if (fnp->f_dirstart == 0)
    {
      bp = getblock(fnp->f_diroff / (secsize / DIRENT_SIZE)
                             + fnp->f_dpb->dpb_dirstrt,
                    fnp->f_dpb->dpb_unit);
#ifdef DISPLAY_GETBLOCK
      printf("DIR (dir_write)\n");
#endif
    }

    /* All other directories are just files. The only       */
    /* special handling is resetting the offset so that we  */
    /* can continually update the same directory entry.     */
    else
    {

      /* Do a "seek" to the directory position        */
      /* and convert the fnode to a directory fnode.  */
      fnp->f_offset = fnp->f_diroff * (ULONG)DIRENT_SIZE;
      fnp->f_cluster = fnp->f_dirstart;
      fnp->f_cluster_offset = 0;

      /* Search through the FAT to find the block     */
      /* that this entry is in.                       */
#ifdef DISPLAY_GETBLOCK
      printf("dir_write: ");
#endif
      if (map_cluster(fnp, XFR_READ) != SUCCESS)
      {
        release_f_node(fnp);
        return FALSE;
      }

      bp = getblock_from_off(fnp, secsize);
      bp->b_flag &= ~(BFR_DATA | BFR_FAT);
      bp->b_flag |= BFR_DIR | BFR_VALID;
#ifdef DISPLAY_GETBLOCK
      printf("DIR (dir_write)\n");
#endif
    }

    /* Now that we have a block, transfer the directory      */
    /* entry into the block.                                */
    if (bp == NULL)
    {
      release_f_node(fnp);
      return FALSE;
    }

    swap_deleted(fnp->f_dir.dir_name);

    putdirent(&fnp->f_dir, &bp->b_buffer[(fnp->f_diroff * DIRENT_SIZE) %
                                         fnp->f_dpb->dpb_secsize]);

    swap_deleted(fnp->f_dir.dir_name);

    bp->b_flag &= ~(BFR_DATA | BFR_FAT);
    bp->b_flag |= BFR_DIR | BFR_DIRTY | BFR_VALID;
  }
  return TRUE;
}
#endif

VOID dir_close(REG f_node_ptr fnp)
{
  /* Test for invalid f_nodes                                     */
  if (fnp == NULL || !(fnp->f_flags & F_DDIR))
    return;

#ifndef IPL
  /* Write out the entry                                          */
  dir_write(fnp);

#endif
  /* Clear buffers after release                                  */
  /* hazard: no error checking! */
  flush_buffers(fnp->f_dpb->dpb_unit);

  /* and release this instance of the fnode                       */
  release_f_node(fnp);
}

#ifndef IPL
COUNT dos_findfirst(UCOUNT attr, BYTE * name)
{
  REG f_node_ptr fnp;
  REG dmatch *dmp = &sda_tmp_dm;
  REG COUNT i;

/*  printf("ff %Fs\n", name);*/

  /* The findfirst/findnext calls are probably the worst of the   */
  /* DOS calls. They must work somewhat on the fly (i.e. - open   */
  /* but never close). Since we don't want to lose fnodes every   */
  /* time a directory is searched, we will initialize the DOS     */
  /* dirmatch structure and then for every find, we will open the */
  /* current directory, do a seek and read, then close the fnode. */

  /* Parse out the file name */
  i = ParseDosName(name, SearchDir.dir_name, TRUE);
  if (i < SUCCESS)
    return i;
/*
  printf("\nff %s", Tname);
  printf("ff %s", fcbname);
*/

  /* Now search through the directory to find the entry...        */

  /* Special handling - the volume id is only in the root         */
  /* directory and only searched for once.  So we need to open    */
  /* the root and return only the first entry that contains the   */
  /* volume id bit set (while ignoring LFN entries).              */
  /* RBIL: ignore ReaDONLY and ARCHIVE bits                       */
  /* For compatibility with bad search requests, only treat as    */
  /*   volume search if only volume bit set, else ignore it.      */
  if ((attr & ~(D_RDONLY | D_ARCHIVE))==D_VOLID)
    i = 3;
  else
    attr &= ~D_VOLID;  /* ignore volume mask */

  /* Now open this directory so that we can read the      */
  /* fnode entry and do a match on it.                    */

/*  printf("dir_open %s\n", szDirName);*/
  {
    char tmp = name[i];
    name[i] = '\0';
    if ((fnp = dir_open(name)) == NULL)
      return DE_PATHNOTFND;
    name[i] = tmp;
  }

  /* Now initialize the dirmatch structure.            */

  dmp->dm_drive = name[0] - 'A';
  dmp->dm_attr_srch = attr;

  /* Copy the raw pattern from our data segment to the DTA. */
  fmemcpy(dmp->dm_name_pat, SearchDir.dir_name, FNAME_SIZE + FEXT_SIZE);

  if ((attr & D_VOLID)==D_VOLID) /* search for VOL label ignore RDONLY & ARCHIVE */
  {
    /* Now do the search                                    */
    while (dir_read(fnp) == 1)
    {
      /* Test the attribute and return first found    */
      if ((fnp->f_dir.dir_attrib & ~(D_RDONLY | D_ARCHIVE)) == D_VOLID &&
          fnp->f_dir.dir_name[0] != DELETED)
      {
        dmp->dm_dircluster = fnp->f_dirstart;   /* TE */
        memcpy(&SearchDir, &fnp->f_dir, sizeof(struct dirent));
        FDirDbgPrintf(("dos_findfirst: %11s\n", fnp->f_dir.dir_name));

        dir_close(fnp);
        return SUCCESS;
      }
      fnp->f_diroff++;
    }

    /* Now that we've done our failed search, close it and  */
    /* return an error.                                     */
    dir_close(fnp);
    return DE_NFILES;
  }
  /* Otherwise just do a normal find next                         */
  else
  {
    dmp->dm_entry = 0;
    dmp->dm_dircluster = fnp->f_dirstart;
    dir_close(fnp);
    return dos_findnext();
  }
}

/*
    BUGFIX TE 06/28/01 
    
    when using FcbFindXxx, the only information available is
    the cluster number + entrycount. everything else MUST\
    be recalculated. 
    a good test for this is MSDOS CHKDSK, which now (seems too) work
*/

COUNT dos_findnext(void)
{
  REG f_node_ptr fnp;
  REG dmatch *dmp = &sda_tmp_dm;

  /* Allocate an fnode if possible - error return (0) if not.     */
  if ((fnp = get_f_node()) == (f_node_ptr) 0)
  {
    return DE_NFILES;
  }

  memset(fnp, 0, sizeof(*fnp));

  /* Force the fnode into read-write mode                         */
  fnp->f_mode = RDWR;

  /* Select the default to help non-drive specified path          */
  /* searches...                                                  */
  fnp->f_dpb = get_dpb(dmp->dm_drive);
  if (media_check(fnp->f_dpb) < 0)
  {
    release_f_node(fnp);
    return DE_NFILES;
  }

  dir_init_fnode(fnp, dmp->dm_dircluster);

  /* Search through the directory to find the entry, but do a     */
  /* seek first.                                                  */
  fnp->f_diroff = dmp->dm_entry;

  /* Loop through the directory                                   */
  while (dir_read(fnp) == 1)
  {
    ++dmp->dm_entry;
    ++fnp->f_diroff;
    if (fnp->f_dir.dir_name[0] != '\0' && fnp->f_dir.dir_name[0] != DELETED &&
      !(fnp->f_dir.dir_attrib & D_VOLID) &&
      fcmp_wild(dmp->dm_name_pat, fnp->f_dir.dir_name, FNAME_SIZE+FEXT_SIZE) &&
      /*
           MSD Command.com uses FCB FN 11 & 12 with attrib set to 0x16.
           Bits 0x21 seem to get set some where in MSD so Rd and Arc
           files are returned. RdOnly + Archive bits are ignored
       */
      /* Test the attribute as the final step */
      !(~dmp->dm_attr_srch & (D_DIR|D_SYSTEM|D_HIDDEN) & fnp->f_dir.dir_attrib))
    {
      /* If found, transfer it to the dmatch structure */
      dmp->dm_dircluster = fnp->f_dirstart;
      memcpy(&SearchDir, &fnp->f_dir, sizeof(struct dirent));
      /* return the result */
      release_f_node(fnp);
      return SUCCESS;
    }
  }
  FDirDbgPrintf(("dos_findnext: %11s\n", fnp->f_dir.dir_name));
  /* return the result                                            */
  release_f_node(fnp);

  return DE_NFILES;
}
#endif
/*
    this receives a name in 11 char field NAME+EXT and builds 
    a zeroterminated string

    unfortunately, blanks are allowed in filenames. like 
        "test e", " test .y z",...
        
    so we have to work from the last blank backward 
*/
void ConvertName83ToNameSZ(BYTE FAR * destSZ, BYTE FAR * srcFCBName)
{
  int loop;

  fmemcpy(destSZ, srcFCBName, FNAME_SIZE);
  for (loop = FNAME_SIZE; --loop >= 0 && destSZ[loop] == ' '; )
    ;
  destSZ += ++loop;
  if (*srcFCBName != '.')             /* not for ".", ".." */
  {
    srcFCBName += FNAME_SIZE;
    for (loop = FEXT_SIZE; --loop >= 0 && srcFCBName[loop] == ' '; )
      ;
    if (++loop > 0)
    {
      *destSZ++ = '.';
      fmemcpy(destSZ, srcFCBName, loop);
      destSZ += loop;
    }
  }
  *destSZ = '\0';
}
