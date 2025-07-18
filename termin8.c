#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <termios.h>
#include <unistd.h>
#include <pty.h>
#include <signal.h>
#include <sys/epoll.h>

#include "termin8.h"

extern char **environ;

struct termios termios_s[1];
struct winsize wins_s[1];

#define MAX_EPOLL_EVENTS 10
struct epoll_event epoll_events[MAX_EPOLL_EVENTS];
struct epoll_event from_stdin;
struct epoll_event from_master;

int master_fd;
int *amaster = &master_fd;

pid_t pid;

#define BUFFER_SIZE 1024
#define MAX_SEQ 80
#define MAX_ESC_BYTES 10

char seq[MAX_SEQ + 1];
char *seq_p;

struct buffer {
  char bytes[BUFFER_SIZE];
  int count;
  int next_start;
  int next_end;
} buffers[2];

FILE *log_file;

enum buffer_type {
  IN_BUF = 0,
  OUT_BUF = 1
};

char *log_prefix[] = { ">>>>>>", "<<<<<<" };

static int min(int i1, int i2) {
  return i1 < i2 ? i1 : i2;
}

static void seq_init() {
  seq_p = seq;
  *seq_p = '\0';
}

static void seq_printf(const char * fmt, ...) {
  va_list ap;
  int max_len = MAX_SEQ - (seq_p - seq);
  va_start(ap, fmt);
  int n = vsnprintf(seq_p, max_len, fmt, ap);
  va_end(ap);
  if(n > max_len) n = max_len;
  seq_p += n;
}

static void seq_bytes(unsigned char *bytes, int n) {
  memcpy(seq_p, bytes, n);
  seq_p += n;
  *seq_p = '\0';
}

static int unknown(char *type, struct buffer *buf, int pos) {
  seq_init();
  seq_printf("Unknown %s: ", type);
  seq_bytes(buf->bytes + pos, min(MAX_ESC_BYTES, buf->count - pos));
  return 1;
}

static int csi_out(struct buffer *buf, int pos) {
  return unknown("CSI", buf, pos);
}

static int osc_out(struct buffer *buf, int pos) {
  return unknown("OSC", buf, pos);
}

static int apc_out(struct buffer *buf, int pos) {
  return unknown("APC", buf, pos);
}

static int escape_in(struct buffer *buf, int pos) {
  return unknown("Escape", buf, pos);
}

static int escape_out(struct buffer *buf, int pos) {
  switch(buf->bytes[pos]) {
    case '[':
      return csi_out(buf, pos + 1);
    case ']':
      return osc_out(buf, pos + 1);
    case '_':
      return apc_out(buf, pos + 1);
  }

  return unknown("Escape", buf, pos);
}

static int escape_sequence(enum buffer_type type, int pos) {
  struct buffer *buf = &buffers[type];
  if(type == IN_BUF) {
    return escape_in(buf, pos);
  } else {
    return escape_out(buf, pos);
  }
}
  

static int next_escape_sequence(enum buffer_type type) {
  struct buffer *buf = &buffers[type];
  int i = buf->next_start;
  while(buf->count > i && 0x1b != buf->bytes[i++] )
    ;
  if(buf->count > i) {
    // found Esc
    if(buf->count > ++i) {
      return escape_sequence(type, i);
    }
  }
  buf->next_end = buf->count;
  return 0;
}
  

static int process_next(enum buffer_type type) {
  struct buffer *buf = &buffers[type];

  buf->next_start = buf->next_end;

  if(log_file) {
    if(next_escape_sequence(type)) {
      fprintf(log_file, "%s\n%s\n", log_prefix[type], seq);
    }
  } else {
    buf->next_end = buf->count;
  }

  return buf->next_end > buf->next_start;
} 

FILE *open_log(const char *log_file) {
  return fopen(log_file, "wb");
}

void disable_raw_mode() {
  tcsetattr(STDIN_FILENO, TCSAFLUSH, termios_s);
}

void flush_all() {
  fflush(stdout);
  if(log_file)
    fflush(log_file);
}

void enable_raw_mode() {
  atexit(disable_raw_mode);

  struct termios raw = termios_s[0];
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void resize(int sig) {
  kill(pid, SIGWINCH);
}

static void copy_bytes(int from_fd, int to_fd, enum buffer_type type) {
  struct buffer *buf = &buffers[type];

  int bytes_read = read(from_fd, buf->bytes + buf->count, BUFFER_SIZE - buf->count);
  if(-1 == bytes_read) {
    perror("read failure in copy_bytes");
    exit(EXIT_FAILURE);
  }
  buf->count += bytes_read;

  buf->next_end = 0;
  while(process_next(type)) {
    int start = buf->next_start;
    int count = buf->next_end - start;
    int bytes_written = 0;
    while(count > bytes_written) {
      int bytes = write(to_fd, buf->bytes + start + bytes_written, count - bytes_written);
      if(-1 == bytes) {
        perror("write failure in copy_bytes");
        exit(EXIT_FAILURE);
      }
      bytes_written += bytes;
    }
  }

  // Copy to start of buf->bytes anything not yet output
  int out_count = buf->next_end;
  buf->count = buf->count - out_count;
  unsigned char *to = buf->bytes;
  unsigned char *from = to + out_count;
  for(int i = 0 ; i < buf->count ; i++, to++, from++) {
    *to = *from;
  }
}

static void master_process(int fd) {
  if(fd == STDIN_FILENO)
    copy_bytes(fd, master_fd, IN_BUF);
  else
    copy_bytes(fd, STDOUT_FILENO, OUT_BUF);
}
    

static void master() { 
  from_stdin.events = EPOLLIN;
  from_stdin.data.fd = STDIN_FILENO;
  from_master.events = EPOLLIN;
  from_master.data.fd = master_fd;

  int epi = epoll_create1(0);
  if(-1 == epi) {
    perror("epoll_create1 failure in master");
    exit(EXIT_FAILURE);
  }
  if(-1 == epoll_ctl(epi, EPOLL_CTL_ADD, STDIN_FILENO, &from_stdin)) {
    perror("epoll_ctl failure on STDIN in master");
    exit(EXIT_FAILURE);
  }
  if(-1 == epoll_ctl(epi, EPOLL_CTL_ADD, master_fd, &from_master)) {
    perror("epoll_ctl failure on master_fd in master");
    exit(EXIT_FAILURE);
  }
  while(1) {
    int res = epoll_wait(epi, epoll_events, MAX_EPOLL_EVENTS, -1);
    if(-1 == res) {
      perror("epoll_wait failure in master");
      exit(EXIT_FAILURE);
    }
    for(int i = 0 ; i < res ; i++) {
      if(EPOLLIN & epoll_events[i].events) {
        master_process(epoll_events[i].data.fd);
      } else if((EPOLLERR | EPOLLHUP) & epoll_events[i].events) {
        return;
      }
    }
  }
}


static void slave() {
  char *cmd = getenv("SHELL");
  char *home = getenv("HOME");
  char *argv[] = { cmd, NULL };
  chdir(home);
  execve(cmd, argv, environ); 
}

int main(int argc, char *argv[]) {
  tcgetattr(STDIN_FILENO, termios_s);
  ioctl(0, TIOCGWINSZ, wins_s);

  pid = forkpty(amaster, NULL, termios_s, wins_s);
  if(-1 == pid) {
    perror("forkpty failure in main");
    exit(EXIT_FAILURE);
  }

  if(pid == 0) {
    slave();
  }

  if(argc = 2) {
    log_file = open_log(argv[1]);
  }
  atexit(flush_all);

  signal(SIGWINCH, resize);
  
  enable_raw_mode();

  master();
}
