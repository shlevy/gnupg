/* asschk.c - Assuan Server Checker
 *	Copyright (C) 2002 Free Software Foundation, Inc.
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

/* This is a simple stand-alone Assuan server test program.  We don't
   want to use the assuan library because we don't want to hide errors
   in that library. 

   The script language is line based.  Empty lines or lines containing
   only white spaces are ignored, line with a hash sign as first non
   white space character are treated as comments.
   
   A simple macro mechanism is implemnted.  Macros are expanded before
   a line is processed but after comment processing.  Macros are only
   expanded once and non existing macros expand to the empty string.
   A macro is dereferenced by prefixing its name with a dollar sign;
   the end of the name is currently indicated by a white space.  To
   use a dollor sign verbatim, double it. 

   A macro is assigned by prefixing a statement with the macro name
   and an equal sign.  The value is assigned verbatim if it does not
   resemble a command, otherwise the return value of the command will
   get assigned.  The command "let" may be used to assign values
   unambigiously and it should be used if the value starts with a
   letter.

   Conditions are not yes implemented except for a simple evaluation
   which yields false for an empty string or the string "0".  The
   result may be negated by prefixing with a '!'.

   The general syntax of a command is:

   [<name> =] <statement> [<args>]

   If NAME is not specifed but the statement returns a value it is
   assigned to the name "?" so that it can be referenced using "$?".
   The following commands are implemented:

   let <value>
      Return VALUE.

   echo <value>
      Print VALUE.

   openfile <filename>
      Open file FILENAME for read access and retrun the file descriptor.

   createfile <filename>
      Create file FILENAME, open for write access and retrun the file
      descriptor.

   pipeserver [<path>]
      Connect to an Assuan server with name PATH.  If PATH is not
      specified the value ../sm/gpgsm is used.

   send <line>
      Send LINE to the server.

   expect-ok
      Expect an OK response from the server.  Status and data out put
      is ignored.

   expect-err
      Expect an ERR response from the server.  Status and data out put
      is ignored.

   quit
      Terminate the process.

   quit-if <condition>
      Terminate the process if CONDITION evaluates to true.

   fail-if <condition>
      Terminate the process with an exit code of 1 if CONDITION
      evaluates to true.

   cmpfiles <first> <second>
      Returns true when the content of the files FIRST and SECOND match.

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>

#if __GNUC__ > 2 || (__GNUC__ == 2 && __GNUC_MINOR__ >= 5 )
# define ATTR_PRINTF(f,a)  __attribute__ ((format (printf,f,a)))
#else
# define ATTR_PRINTF(f,a)
#endif

#define spacep(p) (*(p) == ' ' || *(p) == '\t')

typedef enum {
  LINE_OK = 0,
  LINE_ERR,
  LINE_STAT,
  LINE_DATA,
  LINE_END,
} LINETYPE;


struct variable_s {
  struct variable_s *next;
  int is_fd;
  char *value;
  char name[1];
};
typedef struct variable_s *VARIABLE;



static void die (const char *format, ...)  ATTR_PRINTF(1,2);


/* Name of this program to be printed in error messages. */
static const char *invocation_name;

/* Talk a bit about what is going on. */
static int opt_verbose;

/* File descriptors used to communicate with the current server. */
static int server_send_fd = -1;
static int server_recv_fd = -1;

/* The Assuan protocol limits the line length to 1024, so we can
   safely use a (larger) buffer.  The buffer is filled using the
   read_assuan(). */
static char recv_line[2048];
/* Tell the status of the current line. */
static LINETYPE recv_type;

/* This is our variable storage. */
static VARIABLE variable_list;


static void
die (const char *format, ...)
{
  va_list arg_ptr;

  fflush (stdout);
  fprintf (stderr, "%s: ", invocation_name);

  va_start (arg_ptr, format);
  vfprintf (stderr, format, arg_ptr);
  va_end (arg_ptr);
  putc ('\n', stderr);

  exit (1);
}

static void
err (const char *format, ...)
{
  va_list arg_ptr;

  fflush (stdout);
  fprintf (stderr, "%s: ", invocation_name);

  va_start (arg_ptr, format);
  vfprintf (stderr, format, arg_ptr);
  va_end (arg_ptr);
  putc ('\n', stderr);
}

static void *
xmalloc (size_t n)
{
  void *p = malloc (n);
  if (!p)
    die ("out of core");
  return p;
}

static void *
xcalloc (size_t n, size_t m)
{
  void *p = calloc (n, m);
  if (!p)
    die ("out of core");
  return p;
}

static char *
xstrdup (const char *s)
{
  char *p = xmalloc (strlen (s)+1);
  strcpy (p, s);
  return p;
}


/* Write LENGTH bytes from BUFFER to FD. */
static int
writen (int fd, const char *buffer, size_t length)
{
  while (length)
    {
      int nwritten = write (fd, buffer, length);
      
      if (nwritten < 0)
        {
          if (errno == EINTR)
            continue;
          return -1; /* write error */
        }
      length -= nwritten;
      buffer += nwritten;
    }
  return 0;  /* okay */
}




/* Assuan specific stuff. */

/* Read a line from FD, store it in the global recv_line, analyze the
   type and store that in recv_type.  The function terminates on a
   communication error.  Returns a pointer into the inputline to the
   first byte of the arguments.  The parsing is very strict to match
   excalty what we want to send. */
static char *
read_assuan (int fd)
{
  size_t nleft = sizeof recv_line;
  char *buf = recv_line;
  char *p;
  int nread = 0;

  while (nleft > 0)
    {
      int n = read (fd, buf, nleft);
      if (n < 0)
        {
          if (errno == EINTR)
            continue;
          die ("reading fd %d failed: %s", fd, strerror (errno));
        }
      else if (!n)
        die ("received incomplete line on fd %d", fd);
      p = buf;
      nleft -= n;
      buf += n;
      nread += n;
      
      for (; n && *p != '\n'; n--, p++)
        ;
      if (n)
        {
          /* fixme: keep pending bytes for next read. */
          break;
        }
    }
  if (!nleft)
    die ("received line too large");
  assert (nread>0);
  recv_line[nread-1] = 0;
  
  p = recv_line;
  if (p[0] == 'O' && p[1] == 'K' && (p[2] == ' ' || !p[2]))
    {
      recv_type = LINE_OK;
      p += 3;
    }
  else if (p[0] == 'E' && p[1] == 'R' && p[2] == 'R'
           && (p[3] == ' ' || !p[3]))
    {
      recv_type = LINE_ERR;
      p += 4;
    }
  else if (p[0] == 'S' && (p[1] == ' ' || !p[1]))
    {
      recv_type = LINE_STAT;
      p += 2;
    }
  else if (p[0] == 'D' && p[1] == ' ')
    {
      recv_type = LINE_DATA;
      p += 2;
    }
  else if (p[0] == 'E' && p[1] == 'N' &&  p[2] == 'D' && !p[3])
    {
      recv_type = LINE_END;
      p += 3;
    }
  else 
    die ("invalid line type (%.5s)", p);

  return p;
}

/* Write LINE to the server using FD.  It is expected that the line
   contains the terminating linefeed as last character. */
static void
write_assuan (int fd, const char *line)
{
  char buffer[1026];
  size_t n = strlen (line);

  if (n > 1024)
    die ("line too long for Assuan protocol");
  strcpy (buffer, line);
  if (!n || buffer[n-1] != '\n')
    buffer[n++] = '\n';

  if (writen (fd, buffer, n))
      die ("sending line to %d failed: %s", fd, strerror (errno));
}


/* Start the server with path PGMNAME and connect its stdout and
   strerr to a newly created pipes; the file descriptors are then
   store in the gloabl variables SERVER_SEND_FD and
   SERVER_RECV_FD. The initial handcheck is performed.*/
static void
start_server (const char *pgmname)
{
  int rp[2];
  int wp[2];
  pid_t pid;

  if (pipe (rp) < 0)
    die ("pipe creation failed: %s", strerror (errno));
  if (pipe (wp) < 0)
    die ("pipe creation failed: %s", strerror (errno));

  fflush (stdout);
  fflush (stderr);
  pid = fork ();
  if (pid < 0)
    die ("fork failed");

  if (!pid)
    {
      const char *arg0;

      arg0 = strrchr (pgmname, '/');
      if (arg0)
        arg0++;
      else
        arg0 = pgmname;

      if (wp[0] != STDIN_FILENO)
        {
          if (dup2 (wp[0], STDIN_FILENO) == -1)
              die ("dup2 failed in child: %s", strerror (errno));
          close (wp[0]);
        }
      if (rp[1] != STDOUT_FILENO)
        {
          if (dup2 (rp[1], STDOUT_FILENO) == -1)
              die ("dup2 failed in child: %s", strerror (errno));
          close (rp[1]);
        }

      execl (pgmname, arg0, "--server", NULL); 
      die ("exec failed for `%s': %s", pgmname, strerror (errno));
    }
  close (wp[0]);
  close (rp[1]);
  server_send_fd = wp[1];
  server_recv_fd = rp[0];

  read_assuan (server_recv_fd);
  if (recv_type != LINE_OK)
    die ("no greating message");
}





/* Script intepreter. */

static void
unset_var (const char *name)
{
  VARIABLE var;

  for (var=variable_list; var && strcmp (var->name, name); var = var->next)
    ;
  if (!var)
    return;
/*    fprintf (stderr, "unsetting `%s'\n", name); */

  if (var->is_fd && var->value)
    {
      int fd;

      fd = atoi (var->value);
      if (fd != -1 && fd != 0 && fd != 1 && fd != 2)
          close (fd);
    }

  free (var->value);
  var->value = NULL;
  var->is_fd = 0;
}


static void
set_fd_var (const char *name, const char *value, int is_fd)
{
  VARIABLE var;

  if (!name)
    name = "?"; 
  for (var=variable_list; var && strcmp (var->name, name); var = var->next)
    ;
  if (!var)
    {
      var = xcalloc (1, sizeof *var + strlen (name));
      strcpy (var->name, name);
      var->next = variable_list;
      variable_list = var;
    }
  else
    free (var->value);

  if (var->is_fd && var->value)
    {
      int fd;

      fd = atoi (var->value);
      if (fd != -1 && fd != 0 && fd != 1 && fd != 2)
          close (fd);
    }
  
  var->is_fd = is_fd;
  var->value = xstrdup (value);
/*    fprintf (stderr, "setting `%s' to `%s'\n", var->name, var->value); */

}

static void
set_var (const char *name, const char *value)
{
  set_fd_var (name, value, 0);
}


static const char *
get_var (const char *name)
{
  VARIABLE var;

  for (var=variable_list; var && strcmp (var->name, name); var = var->next)
    ;
  return var? var->value:NULL;
}



/* Expand variables in LINE and return a new allocated buffer if
   required.  The function might modify LINE if the expanded version
   fits into it. */
static char *
expand_line (char *buffer)
{
  char *line = buffer;
  char *p, *pend;
  const char *value;
  size_t valuelen, n;
  char *result = NULL;

  while (*line)
    {
      p = strchr (line, '$');
      if (!p)
        return result; /* nothing more to expand */
      
      if (p[1] == '$') /* quoted */
        {
          memmove (p, p+1, strlen (p+1)+1);
          line = p + 1;
          continue;
        }
      for (pend=p+1; *pend && !spacep (pend) && *pend != '$'; pend++)
        ;
      if (*pend)
        {
          int save = *pend;
          *pend = 0;
          value = get_var (p+1);
          *pend = save;
        }
      else
        value = get_var (p+1);
      if (!value)
        value = "";
      valuelen = strlen (value);
      if (valuelen <= pend - p)
        {
          memcpy (p, value, valuelen);
          p += valuelen;
          n = pend - p;
          if (n)
            memmove (p, p+n, strlen (p+n)+1);
          line = p;
        }
      else
        {
          char *src = result? result : buffer;
          char *dst;

          dst = xmalloc (strlen (src) + valuelen + 1);
          n = p - src;
          memcpy (dst, src, n);
          memcpy (dst + n, value, valuelen);
          n += valuelen;
          strcpy (dst + n, pend);
          line = dst + n;
          free (result);
          result = dst;
        }
    }
  return result;
}


/* Evaluate COND and return the result. */
static int 
eval_boolean (const char *cond)
{
  int true = 1;

  for ( ; *cond == '!'; cond++)
    true = !true;
  if (!*cond || (*cond == '0' && !cond[1]))
    return !true;
  return true;
}





static void
cmd_let (const char *assign_to, char *arg)
{
  set_var (assign_to, arg);
}


static void
cmd_echo (const char *assign_to, char *arg)
{
  printf ("%s\n", arg);
}

static void
cmd_send (const char *assign_to, char *arg)
{
  if (opt_verbose)
    fprintf (stderr, "sending `%s'\n", arg);
  write_assuan (server_send_fd, arg); 
}

static void
cmd_expect_ok (const char *assign_to, char *arg)
{
  if (opt_verbose)
    fprintf (stderr, "expecting OK\n");
  do
    {
      read_assuan (server_recv_fd);
      if (opt_verbose)
        fprintf (stderr, "got line `%s'\n", recv_line);
    }
  while (recv_type != LINE_OK && recv_type != LINE_ERR);
  if (recv_type != LINE_OK)
    die ("expected OK but got `%s'", recv_line);
}

static void
cmd_expect_err (const char *assign_to, char *arg)
{
  if (opt_verbose)
    fprintf (stderr, "expecting ERR\n");
  do
    {
      read_assuan (server_recv_fd);
      if (opt_verbose)
        fprintf (stderr, "got line `%s'\n", recv_line);
    }
  while (recv_type != LINE_OK && recv_type != LINE_ERR);
  if (recv_type != LINE_ERR)
    die ("expected ERR but got `%s'", recv_line);
}

static void
cmd_openfile (const char *assign_to, char *arg)
{
  int fd;
  char numbuf[20];

  do 
    fd = open (arg, O_RDONLY);
  while (fd == -1 && errno == EINTR);
  if (fd == -1)
    die ("error opening `%s': %s", arg, strerror (errno));
  
  sprintf (numbuf, "%d", fd);
  set_fd_var (assign_to, numbuf, 1);
}

static void
cmd_createfile (const char *assign_to, char *arg)
{
  int fd;
  char numbuf[20];

  do 
    fd = open (arg, O_WRONLY|O_CREAT|O_TRUNC, 0666);
  while (fd == -1 && errno == EINTR);
  if (fd == -1)
    die ("error creating `%s': %s", arg, strerror (errno));

  sprintf (numbuf, "%d", fd);
  set_fd_var (assign_to, numbuf, 1);
}


static void
cmd_pipeserver (const char *assign_to, char *arg)
{
  if (!*arg)
    arg = "../sm/gpgsm";

  start_server (arg);
}


static void
cmd_quit_if(const char *assign_to, char *arg)
{
  if (eval_boolean (arg))
    exit (0);
}

static void
cmd_fail_if(const char *assign_to, char *arg)
{
  if (eval_boolean (arg))
    exit (1);
}


static void
cmd_cmpfiles (const char *assign_to, char *arg)
{
  char *p = arg;
  char *second;
  FILE *fp1, *fp2;
  char buffer1[2048]; /* note: both must be of equal size. */
  char buffer2[2048];
  size_t nread1, nread2;
  int rc = 0;

  set_var (assign_to, "0"); 
  for (p=arg; *p && !spacep (p); p++)
    ;
  if (!*p)
    die ("cmpfiles: syntax error");
  for (*p++ = 0; spacep (p); p++)
    ;
  second = p;
  for (; *p && !spacep (p); p++)
    ;
  if (*p)
    {
      for (*p++ = 0; spacep (p); p++)
        ;
      if (*p)
        die ("cmpfiles: syntax error");
    }
  
  fp1 = fopen (arg, "rb");
  if (!fp1)
    {
      err ("can't open `%s': %s", arg, strerror (errno));
      return;
    }
  fp2 = fopen (second, "rb");
  if (!fp2)
    {
      err ("can't open `%s': %s", second, strerror (errno));
      fclose (fp1);
      return;
    }
  while ( (nread1 = fread (buffer1, 1, sizeof buffer1, fp1)))
    {
      if (ferror (fp1))
        break;
      nread2 = fread (buffer2, 1, sizeof buffer2, fp2);
      if (ferror (fp2))
        break;
      if (nread1 != nread2 || memcmp (buffer1, buffer2, nread1))
        {
          rc = 1;
          break;
        }
    }
  if (feof (fp1) && feof (fp2) && !rc)
    {
      if (opt_verbose)
        err ("files match");
      set_var (assign_to, "1"); 
    }
  else if (!rc)
    err ("cmpfiles: read error: %s", strerror (errno));
  else
    err ("cmpfiles: mismatch");
  fclose (fp1);
  fclose (fp2);
}

/* Process the current script line LINE. */
static int
interpreter (char *line)
{
  static struct {
    const char *name;
    void (*fnc)(const char*, char*);
  } cmdtbl[] = {
    { "let"       , cmd_let },
    { "echo"      , cmd_echo },
    { "send"      , cmd_send },
    { "expect-ok" , cmd_expect_ok },
    { "expect-err", cmd_expect_err },
    { "openfile"  , cmd_openfile },
    { "createfile", cmd_createfile },
    { "pipeserver", cmd_pipeserver },
    { "quit"      , NULL },
    { "quit-if"   , cmd_quit_if },
    { "fail-if"   , cmd_fail_if },
    { "cmpfiles"  , cmd_cmpfiles },
    { NULL }
  };
  char *p, *save_p;
  int i, save_c;
  char *stmt = NULL;
  char *assign_to = NULL;
  char *must_free = NULL;

  for ( ;spacep (line); line++)
    ;
  if (!*line || *line == '#')
    return 0; /* empty or comment */
  p = expand_line (line);
  if (p)
    {
      must_free = p;
      line = p;
      for ( ;spacep (line); line++)
        ;
      if (!*line || *line == '#')
        {
          free (must_free);
          return 0; /* empty or comment */
        }
    }

  for (p=line; *p && !spacep (p) && *p != '='; p++)
    ;
  if (*p == '=')
    {
      *p = 0;
      assign_to = line;
    }
  else if (*p)
    {
      for (*p++ = 0; spacep (p); p++)
        ;
      if (*p == '=')
        assign_to = line;
    }
  if (!*line)
    die ("syntax error");
  stmt = line;
  save_c = 0;
  save_p = NULL;
  if (assign_to)
    { /* this is an assignment */
      for (p++; spacep (p); p++)
        ;
      if (!*p)
        {
          unset_var (assign_to);
          free (must_free);
          return 0;
        }
      stmt = p;
      for (; *p && !spacep (p); p++)
        ;
      if (*p)
        {
          save_p = p;
          save_c = *p;
          for (*p++ = 0; spacep (p);  p++)
            ;
        }
    }
  for (i=0; cmdtbl[i].name && strcmp (stmt, cmdtbl[i].name); i++)
    ;
  if (!cmdtbl[i].name)
    {
      if (!assign_to)
        die ("invalid statement `%s'\n", stmt);
      if (save_p)
        *save_p = save_c;
      set_var (assign_to, stmt);
      free (must_free);
      return 0;
    }

  if (cmdtbl[i].fnc)
    cmdtbl[i].fnc (assign_to, p);
  free (must_free);
  return cmdtbl[i].fnc? 0:1;
}



int
main (int argc, char **argv)
{
  char buffer[2048];
  char *p, *pend;

  if (!argc)
    invocation_name = "asschk";
  else
    {
      invocation_name = *argv++;
      argc--;
      p = strrchr (invocation_name, '/');
      if (p)
        invocation_name = p+1;
    }


  set_var ("?","1"); /* defaults to true */

  for (; argc; argc--, argv++)
    {
      p = *argv;
      if (*p != '-')
        break;
      if (!strcmp (p, "--verbose"))
        opt_verbose = 1;
      else if (*p == '-' && p[1] == 'D')
        {
          p += 2;
          pend = strchr (p, '=');
          if (pend)
            {
              int tmp = *pend;
              *pend = 0;
              set_var (p, pend+1);
              *pend = tmp;
            }
          else
            set_var (p, "1");
        }
      else if (*p == '-' && p[1] == '-' && !p[2])
        {
          argc--; argv++;
          break;
        }
      else
        break;
    }
  if (argc)
    die ("usage: asschk [--verbose] {-D<name>[=<value>]}");


  while (fgets (buffer, sizeof buffer, stdin))
    {
      p = strchr (buffer,'\n');
      if (!p)
        die ("incomplete script line");
      *p = 0;
      if (interpreter (buffer))
        break;
      fflush (stdout);
    }
  return 0;
}

