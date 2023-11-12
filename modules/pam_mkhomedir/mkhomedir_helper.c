/* mkhomedir_helper - helper for pam_mkhomedir module

   Released under the GNU LGPL version 2 or later

   Copyright (c) Red Hat, Inc., 2009
   Originally written by Jason Gunthorpe <jgg@debian.org> Feb 1999
   Structure taken from pam_lastlogin by Andrew Morgan
     <morgan@parc.power.net> 1996
 */

#include "config.h"

#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pwd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <syslog.h>

#include <security/pam_ext.h>
#include <security/pam_modutil.h>

static unsigned long u_mask = 0022;
static char skeldir[BUFSIZ] = "/etc/skel";

static int create_homedir(const struct passwd *, mode_t, const char *,
			  const char *);

static int
copy_entry(const struct passwd *pwd, mode_t dir_mode,
	   const char *source, const char *dest, struct dirent *dent)
{
   char remark[BUFSIZ];
   int srcfd = -1, destfd = -1;
   int res;
   int retval = PAM_SESSION_ERR;
   struct stat st;
   char *newsource = NULL, *newdest = NULL;

   /* Determine what kind of file it is. */
   if (asprintf(&newsource, "%s/%s", source, dent->d_name) < 0)
   {
      pam_syslog(NULL, LOG_CRIT, "asprintf failed for 'newsource'");
      retval = PAM_BUF_ERR;
      goto go_out;
   }

   if (lstat(newsource, &st) != 0)
   {
      retval = PAM_SUCCESS;
      goto go_out;
   }

   /* We'll need the new file's name. */
   if (asprintf(&newdest, "%s/%s", dest, dent->d_name) < 0)
   {
      pam_syslog(NULL, LOG_CRIT, "asprintf failed for 'newdest'");
      retval = PAM_BUF_ERR;
      goto go_out;
   }

   /* If it's a directory, recurse. */
   if (S_ISDIR(st.st_mode))
   {
      retval = create_homedir(pwd, dir_mode & (~u_mask), newsource, newdest);
      goto go_out;
   }

   /* If it's a symlink, create a new link. */
   if (S_ISLNK(st.st_mode))
   {
      int pointedlen = 0;
#ifndef PATH_MAX
      char *pointed = NULL;
      {
	 int size = 100;

	 while (1)
	 {
	    pointed = malloc(size);
	    if (pointed == NULL)
	    {
	       retval = PAM_BUF_ERR;
	       goto go_out;
	    }
	    pointedlen = readlink(newsource, pointed, size);
	    if (pointedlen < 0) break;
	    if (pointedlen < size) break;
	    free(pointed);
	    size *= 2;
	 }
      }
      if (pointedlen < 0)
	 free(pointed);
      else
	 pointed[pointedlen] = 0;
#else
      char pointed[PATH_MAX] = {};

      pointedlen = readlink(newsource, pointed, sizeof(pointed) - 1);
#endif

      if (pointedlen >= 0)
      {
	 if (symlink(pointed, newdest) != 0)
         {
	    retval = errno == EEXIST ? PAM_SUCCESS : PAM_PERM_DENIED;

	    if (retval != PAM_SUCCESS)
	       pam_syslog(NULL, LOG_DEBUG,
			  "unable to create link %s: %m", newdest);
#ifndef PATH_MAX
	    free(pointed);
#endif
	    goto go_out;
	 }

	 if (lchown(newdest, pwd->pw_uid, pwd->pw_gid) != 0)
	 {
	    pam_syslog(NULL, LOG_DEBUG,
		       "unable to change perms on link %s: %m", newdest);
#ifndef PATH_MAX
	    free(pointed);
#endif
	    retval = PAM_PERM_DENIED;
	    goto go_out;
	 }
#ifndef PATH_MAX
	 free(pointed);
#endif
      }
      retval = PAM_SUCCESS;
      goto go_out;
   }

   /* If it's not a regular file, it's probably not a good idea to create
    * the new device node, FIFO, or whatever it is. */
   if (!S_ISREG(st.st_mode))
   {
      retval = PAM_SUCCESS;
      goto go_out;
   }

   /* Open the source file */
   if ((srcfd = open(newsource, O_RDONLY)) < 0 || fstat(srcfd, &st) != 0)
   {
      pam_syslog(NULL, LOG_DEBUG,
		 "unable to open or stat src file %s: %m", newsource);
      retval = PAM_PERM_DENIED;
      goto go_out;
   }

   /* Open the dest file */
   if ((destfd = open(newdest, O_WRONLY | O_CREAT | O_EXCL, 0600)) < 0)
   {
      retval = errno == EEXIST ? PAM_SUCCESS : PAM_PERM_DENIED;
      if (retval != PAM_SUCCESS)
         pam_syslog(NULL, LOG_DEBUG,
		    "unable to open dest file %s: %m", newdest);
      goto go_out;
   }

   /* Set the proper ownership and permissions for the module. We make
      the file a+w and then mask it with the set mask. This preserves
      execute bits */
   if (fchmod(destfd, (st.st_mode | 0222) & (~u_mask)) != 0 ||
       fchown(destfd, pwd->pw_uid, pwd->pw_gid) != 0)
   {
      pam_syslog(NULL, LOG_DEBUG,
		 "unable to change perms on copy %s: %m", newdest);
      retval = PAM_PERM_DENIED;
      goto go_out;
   }

   /* Copy the file */
   do
   {
      res = pam_modutil_read(srcfd, remark, sizeof(remark));

      if (res == 0)
	 continue;

      if (res > 0)
      {
	 if (pam_modutil_write(destfd, remark, res) == res)
	    continue;
      }

      /* If we get here, pam_modutil_read returned a -1 or
	 pam_modutil_write returned something unexpected. */
      pam_syslog(NULL, LOG_DEBUG, "unable to perform IO: %m");
      retval = PAM_PERM_DENIED;
      goto go_out;
   }
   while (res != 0);

 go_out:
   if (srcfd >= 0)
      close(srcfd);
   if (destfd >= 0)
      close(destfd);

   free(newsource);
   free(newdest);

   return retval;
}

/* Do the actual work of creating a home dir */
static int
create_homedir(const struct passwd *pwd, mode_t dir_mode,
	       const char *source, const char *dest)
{
   DIR *d = NULL;
   struct dirent *dent;
   int retval = PAM_SESSION_ERR;

   /* Create the new directory */
   if (mkdir(dest, 0700))
   {
      if (errno == EEXIST)
	 return PAM_SUCCESS;
      pam_syslog(NULL, LOG_ERR, "unable to create directory %s: %m", dest);
      return PAM_PERM_DENIED;
   }

   /* See if we need to copy the skel dir over. */
   if ((source == NULL) || (strlen(source) == 0))
   {
      retval = PAM_SUCCESS;
      goto go_out;
   }

   /* Scan the directory */
   d = opendir(source);
   if (d == NULL)
   {
      pam_syslog(NULL, LOG_DEBUG, "unable to read directory %s: %m", source);
      retval = PAM_PERM_DENIED;
      goto go_out;
   }

   for (dent = readdir(d); dent != NULL; dent = readdir(d))
   {
      /* Skip some files.. */
      if (strcmp(dent->d_name,".") == 0 ||
	  strcmp(dent->d_name,"..") == 0)
	 continue;

      retval = copy_entry(pwd, dir_mode, source, dest, dent);
      if (retval != PAM_SUCCESS)
	 goto go_out;
   }

   retval = PAM_SUCCESS;

 go_out:
   if (d != NULL)
      closedir(d);

   if (chmod(dest, dir_mode) != 0 ||
       chown(dest, pwd->pw_uid, pwd->pw_gid) != 0)
   {
      pam_syslog(NULL, LOG_DEBUG,
		 "unable to change perms on directory %s: %m", dest);
      return PAM_PERM_DENIED;
   }

   return retval;
}

static int
create_homedir_helper(const struct passwd *_pwd, mode_t home_mode,
		      const char *_skeldir, const char *_homedir)
{
   int retval = PAM_SESSION_ERR;

   retval = create_homedir(_pwd, home_mode, _skeldir, _homedir);

   return retval;
}

static int
make_parent_dirs(char *dir, int make)
{
  int rc = PAM_SUCCESS;
  char *cp = strrchr(dir, '/');
  struct stat st;

  if (!cp)
    return rc;

  if (cp != dir) {
    *cp = '\0';
    if (stat(dir, &st) && errno == ENOENT)
      rc = make_parent_dirs(dir, 1);
    *cp = '/';

    if (rc != PAM_SUCCESS)
      return rc;
  }

  if (make && mkdir(dir, 0755) && errno != EEXIST) {
    pam_syslog(NULL, LOG_ERR, "unable to create directory %s: %m", dir);
    return PAM_PERM_DENIED;
  }

  return rc;
}

int
main(int argc, char *argv[])
{
   struct passwd *pwd;
   struct stat st;
   char *eptr;
   unsigned long home_mode = 0;

   if (argc < 2) {
	fprintf(stderr, "Usage: %s <username> [<umask> [<skeldir> [<home_mode>]]]\n", argv[0]);
	return PAM_SESSION_ERR;
   }

   pwd = getpwnam(argv[1]);
   if (pwd == NULL) {
	pam_syslog(NULL, LOG_ERR, "User unknown.");
	return PAM_USER_UNKNOWN;
   }

   if (argc >= 3) {
	errno = 0;
	u_mask = strtoul(argv[2], &eptr, 0);
	if (errno != 0 || *eptr != '\0') {
		pam_syslog(NULL, LOG_ERR, "Bogus umask value %s", argv[2]);
		return PAM_SESSION_ERR;
	}
   }

   if (argc >= 4) {
	if (strlen(argv[3]) >= sizeof(skeldir)) {
		pam_syslog(NULL, LOG_ERR, "Too long skeldir path.");
		return PAM_SESSION_ERR;
	}
	strcpy(skeldir, argv[3]);
   }

   if (argc >= 5) {
       errno = 0;
       home_mode = strtoul(argv[4], &eptr, 0);
       if (errno != 0 || *eptr != '\0') {
		pam_syslog(NULL, LOG_ERR, "Bogus home_mode value %s", argv[4]);
		return PAM_SESSION_ERR;
       }
   }

   if (home_mode == 0)
      home_mode = 0777 & ~u_mask;

   /* Stat the home directory, if something exists then we assume it is
      correct and return a success */
   if (stat(pwd->pw_dir, &st) == 0)
	return PAM_SUCCESS;

   if (make_parent_dirs(pwd->pw_dir, 0) != PAM_SUCCESS)
	return PAM_PERM_DENIED;

   return create_homedir_helper(pwd, home_mode, skeldir, pwd->pw_dir);
}
