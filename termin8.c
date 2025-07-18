#include <stdio.h>
#include <stdlib.h>
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

#define iobuf_size 1024
unsigned char iobuf[iobuf_size];

FILE *log_file;

enum log_types {
  LOG_IN = 0,
  LOG_OUT = 1
};

char *log_prefix[] = { "\n<<<<<<\n", "\n>>>>>>\n" };

void log_bytes(enum log_types log_type, int count) {
  if(log_file) {
    fputs(log_prefix[log_type], log_file);
    fwrite(iobuf, 1, count, log_file);
  }
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
  tcgetattr(STDIN_FILENO, termios_s);
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

static int copy_bytes(int from_fd, int to_fd) {
  int bytes_read = read(from_fd, iobuf, iobuf_size);
  if(-1 == bytes_read) {
    perror("read failure in copy_bytes");
    exit(EXIT_FAILURE);
  }
  int bytes_written = 0;
  while(bytes_read > bytes_written) {
    int bytes = write(to_fd, iobuf + bytes_written, bytes_read - bytes_written);
    if(-1 == bytes) {
      perror("write failure in copy_bytes");
      exit(EXIT_FAILURE);
    }
    bytes_written += bytes;
  }
  return bytes_read;
}

static void master_process(int fd) {
  int count;
  if(fd == STDIN_FILENO) {
    count = copy_bytes(fd, master_fd);
    log_bytes(LOG_IN, count);
  } else if(fd == master_fd) {
    count = copy_bytes(fd, STDOUT_FILENO);
    log_bytes(LOG_OUT, count);
  }
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
  if(argc = 2) {
    log_file = open_log(argv[1]);
  }

  atexit(flush_all);

  signal(SIGWINCH, resize);
  
  ioctl(0, TIOCGWINSZ, wins_s);

  enable_raw_mode();

  pid = forkpty(amaster, NULL, termios_s, wins_s);
  if(-1 == pid) {
    perror("forkpty failure in main");
    exit(EXIT_FAILURE);
  }

  if(pid == 0) {
    slave();
  }
  master();
}
