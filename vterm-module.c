#include <emacs-module.h>
#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <vterm.h>

#define MAX(x, y) (((x) > (y)) ? (x) : (y))

/* Declare mandatory GPL symbol.  */
int plugin_is_GPL_compatible;

struct Term {
  VTerm *vt;
  int masterfd;
};

/* Bind NAME to FUN.  */
static void bind_function(emacs_env *env, const char *name, emacs_value Sfun) {
  /* Set the function cell of the symbol named NAME to SFUN using
     the 'fset' function.  */

  /* Convert the strings to symbols by interning them */
  emacs_value Qfset = env->intern(env, "fset");
  emacs_value Qsym = env->intern(env, name);

  /* Prepare the arguments array */
  emacs_value args[] = {Qsym, Sfun};

  /* Make the call (2 == nb of arguments) */
  env->funcall(env, Qfset, 2, args);
}

/* Provide FEATURE to Emacs.  */
static void provide(emacs_env *env, const char *feature) {
  /* call 'provide' with FEATURE converted to a symbol */

  emacs_value Qfeat = env->intern(env, feature);
  emacs_value Qprovide = env->intern(env, "provide");
  emacs_value args[] = {Qfeat};

  env->funcall(env, Qprovide, 1, args);
}

static void message(emacs_env *env, char *message) {
  emacs_value Fmessage = env->intern(env, "message");
  emacs_value string = env->make_string(env, message, strlen(message));
  env->funcall(env, Fmessage, 1, (emacs_value[]){string});
}

static void message_value(emacs_env *env, emacs_value value) {
  emacs_value Fmessage = env->intern(env, "message");
  char *message = "Value: %S";
  emacs_value string = env->make_string(env, message, strlen(message));
  env->funcall(env, Fmessage, 2, (emacs_value[]){string, value});
}

static int string_bytes(emacs_env *env, emacs_value string) {
  ptrdiff_t size = 0;
  env->copy_string_contents(env, string, NULL, &size);
  return size;
}

static emacs_value string_length(emacs_env *env, emacs_value string) {
  emacs_value Flength = env->intern(env, "length");
  return env->funcall(env, Flength, 1, (emacs_value[]){string});
}

static emacs_value list(emacs_env *env, emacs_value *elements, ptrdiff_t len) {
  emacs_value Flist = env->intern(env, "list");
  return env->funcall(env, Flist, len, elements);
}

static void put_text_property(emacs_env *env, emacs_value property,
                              emacs_value value, emacs_value string) {
  emacs_value Fput_text_property = env->intern(env, "put-text-property");
  emacs_value start = env->make_integer(env, 0);
  emacs_value end = string_length(env, string);

  env->funcall(env, Fput_text_property, 5,
               (emacs_value[]){start, end, property, value, string});
}

/*
 * Color must be a string #RGB
 */
static void color_text(emacs_env *env, emacs_value string, emacs_value fg,
                       emacs_value bg) {
  emacs_value foreground = env->intern(env, ":foreground");
  emacs_value background = env->intern(env, ":background");
  emacs_value t = env->intern(env, "t");
  emacs_value face = env->intern(env, "font-lock-face");
  emacs_value value;
  value = list(env, (emacs_value[]){foreground, fg, background, bg}, 4);
  value = list(env, (emacs_value[]){t, value}, 2);
  value = list(env, (emacs_value[]){value}, 1);

  put_text_property(env, face, value, string);
}

static void byte_to_hex(uint8_t byte, char *hex) {
  snprintf(hex, 3, "%.2X", byte);
}

static emacs_value color_to_rgb_string(emacs_env *env, VTermColor color) {
  char buffer[8];
  buffer[0] = '#';
  buffer[7] = '\0';
  byte_to_hex(color.red, buffer + 1);
  byte_to_hex(color.green, buffer + 3);
  byte_to_hex(color.blue, buffer + 5);

  return env->make_string(env, buffer, 7);
};

static void erase_buffer(emacs_env *env) {
  emacs_value Ferase_buffer = env->intern(env, "erase-buffer");
  env->funcall(env, Ferase_buffer, 0, NULL);
}

static void insert(emacs_env *env, emacs_value string) {
  emacs_value Finsert = env->intern(env, "insert");
  env->funcall(env, Finsert, 1, (emacs_value[]){string});
}

static void goto_char(emacs_env *env, int pos) {
  emacs_value Fgoto_char = env->intern(env, "goto-char");
  emacs_value point = env->make_integer(env, pos);
  env->funcall(env, Fgoto_char, 1, (emacs_value[]){point});
}

static void vterm_redraw(VTerm *vt, emacs_env *env) {
  int i, j;
  int rows, cols;
  VTermScreen *screen = vterm_obtain_screen(vt);
  vterm_get_size(vt, &rows, &cols);

  erase_buffer(env);

  for (i = 0; i < rows; i++) {
    for (j = 0; j < cols; j++) {
      VTermPos pos = {.row = i, .col = j};
      VTermScreenCell cell;
      vterm_screen_get_cell(screen, pos, &cell);

      char c;
      if (cell.chars[0] == '\0')
        c = ' ';
      else
        c = cell.chars[0];

      emacs_value string = env->make_string(env, &c, 1);
      emacs_value fg = color_to_rgb_string(env, cell.fg);
      emacs_value bg = color_to_rgb_string(env, cell.bg);
      color_text(env, string, fg, bg);
      insert(env, string);
    }

    insert(env, env->make_string(env, "\n", 1));
  }

  VTermState *state = vterm_obtain_state(vt);
  VTermPos pos;
  vterm_state_get_cursorpos(state, &pos);

  // row * (width + 1) because of newline character
  // col + 1 because (goto-char 1) sets point to first position
  int point = (pos.row * 81) + pos.col + 1;
  goto_char(env, point);
}

static void vterm_flush_output(struct Term *term) {
  size_t bufflen = vterm_output_get_buffer_current(term->vt);
  if (bufflen) {
    char buffer[bufflen];
    bufflen = vterm_output_read(term->vt, buffer, bufflen);

    // TODO: Make work with NON-Blocking io. (buffer in term)
    fcntl(term->masterfd, F_SETFL,
          fcntl(term->masterfd, F_GETFL) & ~O_NONBLOCK);
    write(term->masterfd, buffer, bufflen);
    fcntl(term->masterfd, F_SETFL, fcntl(term->masterfd, F_GETFL) | O_NONBLOCK);
  }
}

static void term_finalize(void *term) {
  vterm_free(((struct Term *)term)->vt);
  free(term);
}

static emacs_value Fvterm_new(emacs_env *env, ptrdiff_t nargs,
                              emacs_value args[], void *data) {
  struct Term *term = malloc(sizeof(struct Term));
  int rows = env->extract_integer(env, args[0]);
  int cols = env->extract_integer(env, args[1]);

  struct winsize size = {rows, cols, 0, 0};

  // Taken almost verbatim from https://bazaar.launchpad.net/~leonerd/pangoterm
  struct termios termios = {
      .c_iflag = ICRNL | IXON,
      .c_oflag = OPOST | ONLCR,
      .c_cflag = CS8 | CREAD,
      .c_lflag = ISIG | ICANON | IEXTEN | ECHO | ECHOE | ECHOK,
      /* c_cc later */
  };

  cfsetspeed(&termios, 38400);

  termios.c_cc[VINTR] = 0x1f & 'C';
  termios.c_cc[VQUIT] = 0x1f & '\\';
  termios.c_cc[VERASE] = 0x7f;
  termios.c_cc[VKILL] = 0x1f & 'U';
  termios.c_cc[VEOF] = 0x1f & 'D';
  termios.c_cc[VEOL] = _POSIX_VDISABLE;
  termios.c_cc[VEOL2] = _POSIX_VDISABLE;
  termios.c_cc[VSTART] = 0x1f & 'Q';
  termios.c_cc[VSTOP] = 0x1f & 'S';
  termios.c_cc[VSUSP] = 0x1f & 'Z';
  termios.c_cc[VREPRINT] = 0x1f & 'R';
  termios.c_cc[VWERASE] = 0x1f & 'W';
  termios.c_cc[VLNEXT] = 0x1f & 'V';
  termios.c_cc[VMIN] = 1;
  termios.c_cc[VTIME] = 0;

  pid_t pid = forkpty(&term->masterfd, NULL, &termios, &size);

  fcntl(term->masterfd, F_SETFL, fcntl(term->masterfd, F_GETFL) | O_NONBLOCK);

  if (pid == 0) {
    char *shell = getenv("SHELL");
    char *args[2] = {shell, NULL};
    execvp(shell, args);
    exit(1);
  }

  term->vt = vterm_new(rows, cols);
  vterm_set_utf8(term->vt, 1);

  VTermScreen *screen = vterm_obtain_screen(term->vt);
  vterm_screen_reset(screen, 1);

  return env->make_user_ptr(env, term_finalize, term);
}

static void process_key(struct Term *term, char *key, VTermModifier modifier) {
  if (strcmp(key, "<return>") == 0) {
    vterm_keyboard_key(term->vt, VTERM_KEY_ENTER, modifier);
  } else if (strcmp(key, "<backspace>") == 0) {
    vterm_keyboard_key(term->vt, VTERM_KEY_BACKSPACE, modifier);
  } else if (strcmp(key, "<tab>") == 0) {
    vterm_keyboard_key(term->vt, VTERM_KEY_TAB, modifier);
  } else if (strcmp(key, "SPC") == 0) {
    vterm_keyboard_unichar(term->vt, " "[0], modifier);
  } else if (strlen(key) == 1) {
    vterm_keyboard_unichar(term->vt, key[0], modifier);
  }

  vterm_flush_output(term);
}

static emacs_value Fvterm_update(emacs_env *env, ptrdiff_t nargs,
                                 emacs_value args[], void *data) {
  struct Term *term = env->get_user_ptr(env, args[0]);
  // Process keys
  if (nargs > 1) {
    ptrdiff_t len = string_bytes(env, args[1]);
    char key[len];
    env->copy_string_contents(env, args[1], key, &len);
    VTermModifier modifier = VTERM_MOD_NONE;
    if (env->is_not_nil(env, args[2]))
      modifier = modifier | VTERM_MOD_SHIFT;
    if (env->is_not_nil(env, args[3]))
      modifier = modifier | VTERM_MOD_ALT;
    if (env->is_not_nil(env, args[4]))
      modifier = modifier | VTERM_MOD_CTRL;

    process_key(term, key, modifier);
  }

  // Read input from masterfd
  char bytes[4096];
  int len;
  if ((len = read(term->masterfd, bytes, 4096)) > 0) {
    vterm_input_write(term->vt, bytes, len);
  };

  vterm_redraw(term->vt, env);
  // TODO: Update screen

  return env->make_integer(env, 0);
}

int emacs_module_init(struct emacs_runtime *ert) {
  emacs_env *env = ert->get_environment(ert);
  emacs_value fun;

  fun =
      env->make_function(env, 2, 2, Fvterm_new, "Allocates a new vterm.", NULL);
  bind_function(env, "vterm-new", fun);

  fun = env->make_function(env, 1, 5, Fvterm_update,
                           "Process io and updates the screen.", NULL);
  bind_function(env, "vterm-update", fun);

  provide(env, "vterm-module");

  return 0;
}