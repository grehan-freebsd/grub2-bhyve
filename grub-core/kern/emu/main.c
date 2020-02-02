/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2003,2004,2005,2006,2007,2008,2009,2010  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <sys/stat.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include <grub/dl.h>
#include <grub/mm.h>
#include <grub/setjmp.h>
#include <grub/fs.h>
#include <grub/emu/hostdisk.h>
#include <grub/time.h>
#include <grub/emu/console.h>
#include <grub/emu/misc.h>
#include <grub/kernel.h>
#include <grub/normal.h>
#include <grub/emu/getroot.h>
#include <grub/env.h>
#include <grub/partition.h>
#include <grub/i18n.h>

#ifdef BHYVE
#include <grub/i386/memory.h>
#include <grub/i386/relocator.h>
#include <grub/emu/bhyve.h>
#endif

#include "progname.h"
#include <argp.h>

#define ENABLE_RELOCATABLE 0

/* Used for going back to the main function.  */
static jmp_buf main_env;

/* Store the prefix specified by an argument.  */
static char *root_dev = NULL, *dir = NULL;

#ifdef BHYVE
static char *grub_cfg = NULL;

#define MB (1024 * 1024)
static char *vmname = NULL;
#endif

int grub_no_autoload;

grub_addr_t grub_modbase = 0;

void
grub_reboot (void)
{
  longjmp (main_env, 1);
}

void
grub_machine_init (void)
{
}

void
grub_machine_get_bootlocation (char **device, char **path)
{
  *device = root_dev;
#ifdef BHYVE
  *path = grub_xasprintf ("%s/%s", dir, grub_cfg);
#else
  *path = dir;
#endif
}

void
grub_machine_fini (void)
{
  grub_console_fini ();
}



static struct argp_option options[] = {
  {"root",      'r', N_("DEVICE_NAME"), 0, N_("Set root device."), 2},
  {"device-map",  'm', N_("FILE"), 0,
   /* TRANSLATORS: There are many devices in device map.  */
   N_("use FILE as the device map [default=%s]"), 0},
  {"directory",  'd', N_("DIR"), 0,
   N_("use GRUB files in the directory DIR [default=%s]"), 0},
  {"verbose",     'v', 0,      0, N_("print verbose messages."), 0},
  {"hold",     'H', N_("SECS"),      OPTION_ARG_OPTIONAL, N_("wait until a debugger will attach"), 0},
#ifdef BHYVE
  {"cons-dev", 'c', N_("cons-dev"), 0, N_("a tty(4) device to use for terminal I/O"), 0},
  {"evga",  'e', 0,            0, N_("exclude VGA rows/cols from bootinfo"), 0},
  {"grub-cfg", 'g', N_("CFG"), 0, N_("alternative name of grub.cfg"), 0},
  {"ncons",  'n', 0,            0, N_("disable insertion of console=ttys0"), 0},
  {"memory", 'M', N_("MBYTES"), 0, N_("guest RAM in MB [default=%d]"), 0},
#endif
  { 0, 0, 0, 0, 0, 0 }
};

static char *
help_filter (int key, const char *text, void *input __attribute__ ((unused)))
{
  switch (key)
    {
    case 'd':
      return xasprintf (text, DEFAULT_DIRECTORY);
    case 'm':
      return xasprintf (text, DEFAULT_DEVICE_MAP);
#ifdef BHYVE
    case 'g':
      return xasprintf (text, DEFAULT_GRUB_CFG);
    case 'M':
      return xasprintf (text, DEFAULT_GUESTMEM);
#endif
    default:
      return (char *) text;
    }
}

struct arguments
{
  const char *dev_map;
  int hold;
#ifdef BHYVE
  grub_uint64_t memsz;
#endif
};

static error_t
argp_parser (int key, char *arg, struct argp_state *state)
{
  /* Get the input argument from argp_parse, which we
     know is a pointer to our arguments structure. */
  struct arguments *arguments = state->input;

  switch (key)
    {
    case 'r':
      free (root_dev);
      root_dev = xstrdup (arg);
      break;
    case 'd':
      free (dir);
      dir = xstrdup (arg);
      break;
    case 'm':
      arguments->dev_map = arg;
      break;
    case 'H':
      arguments->hold = (arg ? atoi (arg) : -1);
      break;
    case 'v':
      verbosity++;
      break;
#ifdef BHYVE
    case 'c':
      grub_emu_bhyve_set_console_dev(xstrdup(arg));
      break;
    case 'e':
      grub_emu_bhyve_unset_vgainsert();
      break;
    case 'g':
      free (grub_cfg);
      grub_cfg = xstrdup(arg);
      break;
    case 'n':
      grub_emu_bhyve_unset_cinsert();
      break;
    case 'M':
      if (grub_emu_bhyve_parse_memsize(arg, &arguments->memsz) != 0) {
	fprintf (stderr, _("Invalid guest memory size `%s'."), arg);
	fprintf (stderr, "\n");
	return EINVAL;
      }
      break;
#endif

    case ARGP_KEY_ARG:
      {
#ifdef BHYVE
	/* The name of the vm */
	vmname = xstrdup (arg);
#else
	/* Too many arguments. */
	fprintf (stderr, _("Unknown extra argument `%s'."), arg);
	fprintf (stderr, "\n");
	argp_usage (state);
#endif
      }
      break;

    default:
      return ARGP_ERR_UNKNOWN;
    }
  return 0;
}

static struct argp argp = {
#ifdef BHYVE
  options, argp_parser, "vmname",
  N_("grub-bhyve boot loader."),
#else
  options, argp_parser, NULL,
  N_("GRUB emulator."),
#endif
  NULL, help_filter, NULL
};

#ifdef BHYVE
/*
 * Represent run-time conditional options as argp child options.
 * The only one at this point is the "-S" option to force wiring
 * of guest memory on >= 11.0-r284539.
 */
static struct argp_option bhyve_options[] = {
  {0, 'S', 0, 0, N_("Force wiring of guest memory."), 0},
  { 0, 0, 0, 0, 0, 0 }
};

static error_t
bhyve_opt_parser (int key, char *arg, struct argp_state *state)
{

  switch (key)
    {
    case 'S':
      grub_emu_bhyve_set_memwire();
      break;
    default:
      return ARGP_ERR_UNKNOWN;
    }
  return 0;
}

static struct argp bhyve_opt_argp = {
  bhyve_options,
  bhyve_opt_parser
};

static struct argp_child bhyve_opt_child = {
  &bhyve_opt_argp,
  0,
  "",
  2
};

static struct argp_child bhyve_argp_children[2];
#endif



void grub_hostfs_init (void);
void grub_hostfs_fini (void);
void grub_host_init (void);
void grub_host_fini (void);
void grub_emu_init (void);

int
main (int argc, char *argv[])
{
  struct arguments arguments =
    { 
      .dev_map = DEFAULT_DEVICE_MAP,
      .hold = 0
#ifdef BHYVE
	,
      .memsz = DEFAULT_GUESTMEM * MB
#endif
    };
  volatile int hold = 0;

  set_program_name (argv[0]);

  dir = xstrdup (DEFAULT_DIRECTORY);

#ifdef BHYVE
  grub_cfg = xstrdup (DEFAULT_GRUB_CFG);

  if (grub_emu_bhyve_memwire_avail()) {
    bhyve_argp_children[0] = bhyve_opt_child;
    bhyve_argp_children[1].argp = NULL;
    argp.children = bhyve_argp_children;
  }    
#endif

  if (argp_parse (&argp, argc, argv, 0, 0, &arguments) != 0)
    {
      fprintf (stderr, "%s", _("Error in parsing command line arguments\n"));
      exit(1);
    }

#ifdef BHYVE
  if (vmname == NULL)
    {
      char buf[80];
      fprintf (stderr, "%s", _("Required VM name parameter not supplied\n"));
      argp_help (&argp, stderr, ARGP_HELP_SEE, buf);
      exit(1);
    }

  if (grub_emu_bhyve_init(vmname, arguments.memsz) != 0)
    {
      fprintf (stderr, "%s", _("Error in initializing VM\n"));
      exit(1);
    }
#endif

  hold = arguments.hold;
  /* Wait until the ARGS.HOLD variable is cleared by an attached debugger. */
  if (hold && verbosity > 0)
    /* TRANSLATORS: In this case GRUB tells user what he has to do.  */
    printf (_("Run `gdb %s %d', and set ARGS.HOLD to zero.\n"),
            program_name, (int) getpid ());
  while (hold)
    {
      if (hold > 0)
        hold--;

      sleep (1);
    }

  signal (SIGINT, SIG_IGN);
  grub_emu_init ();
  grub_console_init ();
  grub_host_init ();

  /* XXX: This is a bit unportable.  */
  grub_util_biosdisk_init (arguments.dev_map);

  grub_init_all ();

  grub_hostfs_init ();

  grub_emu_post_init ();
#ifdef BHYVE
  /* Drop privileges and sandbox. */
  grub_emu_bhyve_post_init ();
#endif

  /* Make sure that there is a root device.  */
  if (! root_dev)
    root_dev = grub_strdup ("host");

  dir = xstrdup (dir);

  /* Start GRUB!  */
  if (setjmp (main_env) == 0)
    grub_main ();

  grub_fini_all ();
  grub_hostfs_fini ();
  grub_host_fini ();

  grub_machine_fini ();

  return 0;
}

#ifdef __MINGW32__

void
grub_millisleep (grub_uint32_t ms)
{
  Sleep (ms);
}

#else

void
grub_millisleep (grub_uint32_t ms)
{
  struct timespec ts;

  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (ms % 1000) * 1000000;
  nanosleep (&ts, NULL);
}

#endif
