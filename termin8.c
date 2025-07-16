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

void disable_raw_mode() {
  tcsetattr(STDIN_FILENO, TCSAFLUSH, termios_s);
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

static void copy_bytes(int from_fd, int to_fd) {
  int VEXOF(bytes_read, read(from_fd, iobuf, iobuf_size));
  int bytes_written = 0;
  while(bytes_read > bytes_written) {
    int VEXOF(bytes, write(to_fd, iobuf + bytes_written, bytes_read - bytes_written));
    bytes_written += bytes;
  }
}

static void master_process(int fd) {
  if(fd == STDIN_FILENO) {
    copy_bytes(fd, master_fd);
  } else if(fd == master_fd) {
    copy_bytes(fd, STDOUT_FILENO);
  }
}
    

static void master() { 
  from_stdin.events = EPOLLIN;
  from_stdin.data.fd = STDIN_FILENO;
  from_master.events = EPOLLIN;
  from_master.data.fd = master_fd;

  int VEXOF(epi, epoll_create1(0));
  EXOF(epoll_ctl(epi, EPOLL_CTL_ADD, STDIN_FILENO, &from_stdin));
  EXOF(epoll_ctl(epi, EPOLL_CTL_ADD, master_fd, &from_master));
  while(1) {
    int VEXOF(res, epoll_wait(epi, epoll_events, MAX_EPOLL_EVENTS, -1));
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
  char *argv[] = { cmd, NULL };
  execve(cmd, argv, environ); 
}

int main(int argc, char *argv[]) {
  signal(SIGWINCH, resize);
  
  ioctl(0, TIOCGWINSZ, wins_s);

  enable_raw_mode();

  VEXOF(pid, forkpty(amaster, NULL, termios_s, wins_s));

  if(pid == 0) {
    slave();
  }
  master();
}
