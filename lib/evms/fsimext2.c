/*
 *
 *   Copyright (c) International Business Machines  Corp., 2000
 *
 *   This program is free software;  you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY;  without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See
 *   the GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program;  if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 *   Module: fsimext2.c
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <plugin.h>
#include "fsimext2.h"

int fsim_rw_diskblocks( int, int64_t, int32_t, void *, int );
void set_mkfs_options( option_array_t *, char **, logical_volume_t *, char * );
void set_fsck_options( option_array_t *, char **, logical_volume_t * );

// Vector of plugin record ptrs that we export for the EVMS Engine. 
plugin_record_t *evms_plugin_records[] = {
	&ext2_plugrec,
	NULL
};

static plugin_record_t  * pMyPluginRecord = &ext2_plugrec;

/*-------------------------------------------------------------------------------------+
+                                                                                      +
+                                   Common Routines                                    +
+                                                                                      +
+--------------------------------------------------------------------------------------*/


/*
 * Get the size limits for this volume.
 */
int fsim_get_volume_limits( struct ext2_super_block * sb,
			   sector_count_t    * min_size,
			   sector_count_t    * max_volume_size,
			   sector_count_t    * max_object_size)
{
	int rc = 0;
	sector_count_t fs_size;

	/* 
	 * Since ext2/3 does not yet support shrink or expand,
	 * all values are actual file system size.
	 */
	fs_size = sb->s_blocks_count << (1 + sb->s_log_block_size);
	*max_volume_size = fs_size;
	*max_object_size = fs_size;
	*min_size        = fs_size;

	return rc;
}


/*
 * Un-Format the volume.
 */
int fsim_unmkfs( logical_volume_t * volume )
{
    int  fd;
    int  rc = 0;

    fd = open(EVMS_GET_DEVNAME(volume), O_RDWR|O_EXCL, 0);
    if (fd < 0) return -1;

    if ( volume->private_data ) {
        /* clear private data */
        memset( (void *) volume->private_data, 0, SIZE_OF_SUPER );
        /* zero primary superblock */
        rc =  fsim_rw_diskblocks( fd, EXT2_SUPER_LOC, SIZE_OF_SUPER,
				 volume->private_data, PUT );
    } else {
        rc = ERROR;
    }

    fd = close(fd);

    return rc;
}


/*
 * Format the volume.
 */
int fsim_mkfs(logical_volume_t * volume, option_array_t * options )
{
	int     rc = FSIM_ERROR;
	char   *argv[MKFS_EXT2_OPTIONS_COUNT + 6];
	char    logsize[sizeof(unsigned int) + 1];
	pid_t	pidm;
	int     status;

	/* Fork and execute the correct program. */
    switch (pidm = fork()) {
        
        /* error */
        case -1:
        	return EIO;

        /* child */
        case 0:  
            set_mkfs_options( options, argv, volume, logsize );

            /* close stderr, stdout to suppress mke2fs output */
            close(1);
            close(2);
            open("/dev/null", O_WRONLY);
            open("/dev/null", O_WRONLY);

            (void) execvp(argv[0], argv);
            /* using exit() can hang GUI, use _exit */
            _exit(errno);

        /* parent */
        default:
            /* wait for child to complete */
            pidm = waitpid( pidm, &status, 0 );
            if ( WIFEXITED(status) ) {
                /* get mke2fs exit code */
                rc = WEXITSTATUS(status);
            }
    }

    return rc;
}


/*
 * NAME: set_mkfs_options
 *
 * FUNCTION: Build options array (argv) for mkfs.ext2
 *
 * PARAMETERS:
 *      options   - options array passed from EVMS engine
 *      argv      - mkfs options array
 *      vol_name  - volume name on which program will be executed
 *
 */                        
void set_mkfs_options( option_array_t * options, 
                       char ** argv, 
                       logical_volume_t * volume, 
                       char * logsize )
{
    int i, opt_count = 2;

    argv[0] = "mke2fs";

    /* 'quiet' option */
    argv[1] = "-q";

    for ( i=0; i<options->count; i++ ) {

        if ( options->option[i].is_number_based ) {

            switch (options->option[i].number) {
                
            case MKFS_CHECKBB_INDEX:
                /* 'check for bad blocks' option */
                if ( options->option[i].value.bool == TRUE ) {
                    argv[opt_count++] = "-c";
                }
                break;

            case MKFS_CHECKRW_INDEX:
                /* 'check for r/w bad blocks' option */
                if ( options->option[i].value.bool == TRUE ) {
                    argv[opt_count++] = "-cc";
                }
                break;

            case MKFS_JOURNAL_INDEX:
                /* 'create ext3 journal' option */
                if ( options->option[i].value.bool == TRUE ) {
                    argv[opt_count++] = "-j";
                }
                break;

            case MKFS_SETVOL_INDEX:
                /* 'set volume name' option */
                if ( options->option[i].value.s ) {
                    argv[opt_count++] = "-L";
                    argv[opt_count++] = options->option[i].value.s;
                }
                break;

            default:
                break;
            }

        } else {

            if ( !strcmp(options->option[i].name, "badblocks") ) {
                /* 'check for bad blocks' option */
                if ( options->option[i].value.bool == TRUE ) {
                    argv[opt_count++] = "-c";
                }
            }

            if ( !strcmp(options->option[i].name, "badblocks_rw") ) {
                /* 'check for r/w bad blocks' option */
                if ( options->option[i].value.bool == TRUE ) {
                    argv[opt_count++] = "-cc";
                }
            }

            if ( !strcmp(options->option[i].name, "journal") ) {
                /* 'create ext3 journal' option */
                if ( options->option[i].value.bool == TRUE ) {
                    argv[opt_count++] = "-j";
                }
            }

            if ( !strcmp(options->option[i].name, "vollabel") ) {
                /* 'check for bad blocks' option */
                if ( options->option[i].value.s ) {
                    argv[opt_count++] = "-L";
                    argv[opt_count++] = options->option[i].value.s;
                }
            }
        }
    }

    argv[opt_count++] = EVMS_GET_DEVNAME(volume);
    argv[opt_count] = NULL;
     
    {
	    FILE	*f;

	    f = fopen("/var/tmp/evms-log", "a");
	    for ( i=0; argv[i]; i++) {
		    fprintf(f, "'%s' ", argv[i]);
	    }
	    fprintf(f, "\n");
	    fclose(f);
    }
    
    return;
}


/*
 * Run fsck on the volume.
 */
int fsim_fsck(logical_volume_t * volume, option_array_t * options )
{
	int     rc = FSIM_ERROR;
	char   *argv[FSCK_EXT2_OPTIONS_COUNT + 3];
	pid_t	pidf;
	int     status, bytes_read;
	char    *buffer = NULL;
	int     fds2[2];
	int	banner = 0;

	/* open pipe, alloc buffer for collecting fsck.jfs output */
	rc = pipe(fds2);
	if (rc) {
	    return(rc);
	}
	if (!(buffer = EngFncs->engine_alloc(MAX_USER_MESSAGE_LEN))) {
	    return(ENOMEM);
	}

	/* Fork and execute the correct program. */
	switch (pidf = fork()) {
        
        /* error */
        case -1:
        	return EIO;

        /* child */
        case 0:  
            set_fsck_options( options, argv, volume );

            /* pipe stderr, stdout */
		    dup2(fds2[1],1);	/* fds2[1] replaces stdout */
		    dup2(fds2[1],2);  	/* fds2[1] replaces stderr */
		    close(fds2[0]);	/* don't need this here */

            rc = execvp( argv[0], argv );

            /*
             * The value of fsck exit code FSCK_CORRECTED is the same
             * as errno EPERM.  Thus, if EPERM is returned from execv,
             * exit out of the child with rc = -1 instead of EPERM.
             */
            if( rc && (errno == EPERM) ) {
                /* using exit() can hang GUI, use _exit */
                _exit(-1);
            } else {
                /* using exit() can hang GUI, use _exit */
                _exit(errno);
            }

        /* parent */
        default:
		close(fds2[1]);

		/* wait for child to complete */
		while (!(pidf = waitpid( pidf, &status, WNOHANG ))) {
			/* read e2fsck output */
			bytes_read = read(fds2[0],buffer,MAX_USER_MESSAGE_LEN);
			if (bytes_read > 0) {
				/* display e2fsck output */
				if (!banner)
					MESSAGE("e2fsck output: \n\n%s",buffer);
				else
					banner = 1;
				memset(buffer,0,bytes_read); //clear out message  
			}
			usleep(10000); /* don't hog all the cpu */
		}

		/* wait for child to complete */
		pidf = waitpid( pidf, &status, 0 );
		if ( WIFEXITED(status) ) {
			/* get e2fsck exit code */
			rc = WEXITSTATUS(status);
			LOG("e2fsck completed with exit code %d \n", rc);
		}
	}

	if (buffer) {
		EngFncs->engine_free(buffer);
	}

    return rc;
}


/*
 * NAME: set_fsck_options
 *
 * FUNCTION: Build options array (argv) for e2fsck
 *
 * PARAMETERS:
 *      options   - options array passed from EVMS engine
 *      argv      - fsck options array
 *      volume    - volume on which program will be executed
 *
 */                        
void set_fsck_options( option_array_t * options, char ** argv, logical_volume_t * volume )
{
    int i, opt_count = 1;
    int do_preen = 1;

    argv[0] = "e2fsck";

    for ( i=0; i<options->count; i++) {

        if ( options->option[i].is_number_based ) {

            /* 'force check' option */
            if ( (options->option[i].number == FSCK_FORCE_INDEX) && 
                 (options->option[i].value.bool == TRUE) ) {
                argv[opt_count++] = "-f";
            }

            /* 'check read only' option or mounted */
            if ((options->option[i].number == FSCK_READONLY_INDEX) &&
		((options->option[i].value.bool == TRUE) ||
		 EVMS_IS_MOUNTED(volume))) {
                argv[opt_count++] = "-n";
		do_preen = 0;
            }

            /* 'bad blocks check' option and NOT mounted */
            if ( (options->option[i].number == FSCK_CHECKBB_INDEX) && 
                 (options->option[i].value.bool == TRUE)         &&
                 !EVMS_IS_MOUNTED(volume) ) {
                argv[opt_count++] = "-c";
		do_preen = 0;
            }

            /* 'bad blocks check' option and NOT mounted */
            if ( (options->option[i].number == FSCK_CHECKRW_INDEX) && 
                 (options->option[i].value.bool == TRUE)         &&
                 !EVMS_IS_MOUNTED(volume) ) {
                argv[opt_count++] = "-cc";
		do_preen = 0;
            }
	    
            /* timing option */
            if ( (options->option[i].number == FSCK_TIMING_INDEX) && 
		 (options->option[i].value.bool == TRUE) ) {
                argv[opt_count++] = "-tt";
            }
	    
    } else {

            /* 'force check' option selected and NOT mounted */
            if ( !strcmp(options->option[i].name, "force") &&
                 (options->option[i].value.bool == TRUE) &&
                 !EVMS_IS_MOUNTED(volume) ) {
                argv[opt_count++] = "-f";
            }

            /* 'check read only' option selected or mounted */
            if ((!strcmp(options->option[i].name, "readonly")) &&
		((options->option[i].value.bool == TRUE) ||
                 EVMS_IS_MOUNTED(volume))) {
                argv[opt_count++] = "-n";
		do_preen = 0;
            }

            /* 'check badblocks' option selected and NOT mounted */
            if (!strcmp(options->option[i].name, "badblocks") &&
		(options->option[i].value.bool == TRUE) &&
		!EVMS_IS_MOUNTED(volume)) {
                argv[opt_count++] = "-c";
		do_preen = 0;
            }

            /* 'check r/w badblocks' option selected and NOT mounted */
            if (!strcmp(options->option[i].name, "badblocks_rw") &&
		(options->option[i].value.bool == TRUE) &&
		!EVMS_IS_MOUNTED(volume)) {
                argv[opt_count++] = "-cc";
		do_preen = 0;
            }

            /* 'timing' option selected */
            if (!strcmp(options->option[i].name, "badblocks") &&
		(options->option[i].value.bool == TRUE)) {
                argv[opt_count++] = "-tt";
            }
        }
    }

    if (do_preen)
	    argv[opt_count++] = "-p";
    argv[opt_count++] = EVMS_GET_DEVNAME(volume);
    argv[opt_count]   = NULL;

    {
	    FILE	*f;

	    f = fopen("/var/tmp/evms-log", "a");
	    for ( i=0; argv[i]; i++) {
		    fprintf(f, "'%s' ", argv[i]);
	    }
	    fprintf(f, "\n");
	    fclose(f);
    }
    
    return;
}


/*
 * NAME: fsim_get_ext2_superblock
 *
 * FUNCTION: Get and validate a ext2/3 superblock
 *
 * PARAMETERS:
 *      volume   - pointer to volume from which to get the superblock
 *      sb_ptr   - pointer to superblock
 *
 * RETURNS:
 *      (0) for success
 *      != 0 otherwise
 *        
 */                        
int fsim_get_ext2_superblock( logical_volume_t *volume, struct ext2_super_block *sb_ptr )
{
    int  fd;
    int  rc = 0;

    fd = open(EVMS_GET_DEVNAME(volume), O_RDONLY, 0);
    if (fd < 0) return EIO;

    /* get primary superblock */
    rc = fsim_rw_diskblocks( fd, EXT2_SUPER_LOC, SIZE_OF_SUPER, sb_ptr, GET );

    if( rc == 0 ) {
        /* see if superblock is ext2/3 */
        if (( sb_ptr->s_magic != EXT2_SUPER_MAGIC ) ||
	    ( sb_ptr->s_rev_level > 1 ))
		rc = FSIM_ERROR;
    }

    close(fd);

    return rc;
}


/*
 * NAME: fsim_rw_diskblocks
 *
 * FUNCTION: Read or write specific number of bytes for an opened device.
 *
 * PARAMETERS:
 *      dev_ptr         - file handle of an opened device to read/write
 *      disk_offset     - byte offset from beginning of device for start of disk
 *                        block read/write
 *      disk_count      - number of bytes to read/write
 *      data_buffer     - On read this will be filled in with data read from
 *                        disk; on write this contains data to be written
 *      mode            - GET (read) or PUT (write)
 *
 * RETURNS:
 *      FSIM_SUCCESS (0) for success
 *      ERROR       (-1) can't lseek
 *      EINVAL           
 *      EIO
 *        
 */                        
int fsim_rw_diskblocks( int      dev_ptr,
                        int64_t  disk_offset,
                        int32_t  disk_count,
                        void     *data_buffer,
                        int      mode )
{
    off_t   Actual_Location;
    size_t  Bytes_Transferred;

    Actual_Location = lseek(dev_ptr,disk_offset, SEEK_SET);
    if ( ( Actual_Location < 0 ) || ( Actual_Location != disk_offset ) )
        return ERROR;

    switch ( mode ) {
        case GET:
            Bytes_Transferred = read(dev_ptr,data_buffer,disk_count);
            break;
        case PUT:
            Bytes_Transferred = write(dev_ptr,data_buffer,disk_count);
            break;
        default:
            return EINVAL;
            break;
    }

    if ( Bytes_Transferred != disk_count ) {
        return EIO;
    }

    return FSIM_SUCCESS;
}


/*
 * Test e2fsprogs version.
 *
 * We don't bother since we don't need any special functionality that
 * hasn't been around for *years*
 */	
int fsim_test_version( )
{
	return 0;
}





