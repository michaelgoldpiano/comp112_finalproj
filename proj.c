#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <time.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>

#define REQUEST_BUFFER_SIZE 100000
#define EACH_HEADER_SIZE 200
#define RESPONSE_MSG_SIZE 10000000
#define DEFAULT_CACHE_AGE 3600
#define CACHE_CAP 20
#define FD_TIMEOUT_SECONDS 60
#define RD_WR_BUFFER_SIZE 100000

typedef struct {
  int fd_id;
  char *buffer;
  int size;
  int size_written;
} rd_wr_buffer;
rd_wr_buffer **rd_wr_buffers;
rd_wr_buffer **write_buffer_lookup;

typedef struct {
  bool is_get_request;
  int port_num;
  char host_name[EACH_HEADER_SIZE]; // '\0' terminated
  char request_method[EACH_HEADER_SIZE]; // '\0' terminated
  char msg[REQUEST_BUFFER_SIZE]; // '\0' terminated
} request_from_client;

typedef struct {
  int body_len;
  int msg_len;
  char *msg;
  char *body;
} response_from_server;

typedef struct {
  int id;
  char key[EACH_HEADER_SIZE]; // '\0' terminated
  response_from_server *value;
  long expiration_time;
} cache_ele;
cache_ele **cache_arr;

long *fd_last_mod_time;


void init();
void rd_wr_buf_init();
void rd_wr_buf_new(int fd_id);
void rd_wr_buf_reset(rd_wr_buffer *buf);
int make_socket(uint16_t port);
bool is_request_end(char *str, int len);
int parse_port_number(char *str);
request_from_client* request_parse(rd_wr_buffer *req_buf);
bool contains_chars(char *ptr, char *header, int len);
void cache_init();
cache_ele* cache_new();
void cache_reset(cache_ele *cache);
response_from_server* response_new();
void response_reset(response_from_server *response);
cache_ele* cache_find(char *key);
cache_ele* cache_next_free();
cache_ele* cache_new_or_reset(cache_ele* node, request_from_client *req);
void response_parse(cache_ele *node, int len);
void process_client_request(int fd_id);
rd_wr_buffer* process_get_request(request_from_client *req, rd_wr_buffer* write_buf);
rd_wr_buffer* process_connect_request(request_from_client *req, rd_wr_buffer* write_buf);
int connect_server(request_from_client *req);

// TO be implemented
void cache_refresh(); // active cache reset
void destory();


int main(int argc, char **argv) {
  int main_sock;
  struct sockaddr_in clientname;
  int proxy_port_no; // port to listen on
  
  /* 
   * check command line arguments
   */
  if (argc != 2) {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }
  proxy_port_no = atoi(argv[1]);
  
  /* Create the socket and set it up to accept connections. */
  main_sock = make_socket(proxy_port_no);
  if (listen(main_sock, 1) < 0) {
    perror("listen");
    exit(EXIT_FAILURE);
  }
  
  init();
  
  // init timeout
  
  fd_set read_fd_set, write_fd_set;
  /* run the proxy */
  while (1) {
    int max_fd = main_sock;
    FD_ZERO(&read_fd_set);
    FD_ZERO(&write_fd_set);
    FD_SET(STDIN_FILENO, &read_fd_set);
    FD_SET(main_sock, &read_fd_set);
    long cur_time = time(NULL);
    
    // init read and write set and max_fd
    for (int i = 1; i < FD_SETSIZE; ++i) {
      // ignore fd_id with no connection
      long fd_time = fd_last_mod_time[i];
      if (fd_time == 0) {
        continue;
      }
      
      rd_wr_buffer *read_buf = rd_wr_buffers[i];
      rd_wr_buffer *write_buf = write_buffer_lookup[i];
      int cache_index = read_buf->fd_id - FD_SETSIZE;
      
      // reset buffer if timeout for interaction with proxy
      if (cur_time - fd_time >= FD_TIMEOUT_SECONDS) {
        shutdown(i, SHUT_RDWR);
        close(i);
        fd_last_mod_time[i] = 0;
        write_buffer_lookup[i] = NULL;
        
        if (cache_index >= 0) {
          cache_reset(cache_arr[cache_index]);
          free(read_buf);
        } else {
          rd_wr_buf_reset(read_buf);
        }
        
        continue;
      }
        
      // set read when there is space
      if (read_buf->size < RD_WR_BUFFER_SIZE || cache_index >= 0) {
        FD_SET(i, &read_fd_set);
        max_fd = max_fd > i ? max_fd : i;
      }
      
      // set write if possible
      if (write_buf != NULL && write_buf->size > write_buf->size_written) {
        FD_SET(i, &write_fd_set);
        max_fd = max_fd > i ? max_fd : i;
      }
    }
    
    /* Initialize the Timeout */
    struct timeval interval; // refresh for each interval
    interval.tv_sec = 1;
    interval.tv_usec = 0;
    
    /* Block until input arrives on one or more active sockets */
    if (select(max_fd + 1, &read_fd_set, &write_fd_set, NULL, &interval) < 0) {

      perror("select");
      exit(EXIT_FAILURE);
    }

    /* Service all the sockets with input pending or close any socket w partial
       data >= 1min */
    
    // serve all active fd_id
    for (int i = 0; i <= max_fd; ++i) {
      int num_bytes;

      if (i == STDIN_FILENO) {
        if (FD_ISSET(STDIN_FILENO, &read_fd_set)) {
          int buf_len = 1000;
          char *buf = calloc(buf_len, sizeof(*buf));
          int read_len = read(STDIN_FILENO, buf, buf_len);
          buf[read_len - 1] = '\0';  /* Replace enter-key '\n' with '\0' */

          /* command EXIT */
          if (strcmp(buf, "EXIT") == 0) {
            exit(EXIT_SUCCESS);
          }

          /* search */
          int num_results_found = 0;
          printf("**********\n");

          for (int i = 0; i < CACHE_CAP; i++) {
            /* Key (URI) */
            char *key = cache_arr[i]->key;

            /* Val (Body) */
            int body_len = cache_arr[i]->value->body_len;
            char *body = malloc((sizeof(*body) * body_len) + 1);
            body[body_len] = '\0';
            memcpy(body, cache_arr[i]->value->body, body_len);

            /* Print */
            if (strstr(body, buf) != NULL) {
              num_results_found++;
              printf("%s\n", key);
              printf("%s\n", body);
              printf("**********\n");
            }
          }
          if (num_results_found == 0) {
            printf("No results found for \"%s\".\n", buf);
          } else {
            printf("%d results found for \"%s\".\n", num_results_found, buf);
          }

          free(buf);
        }
        continue;
      }
      
      else if (i == main_sock) {
        /* Connection request on original/main socket. */
        if (FD_ISSET(main_sock, &read_fd_set)) {
          int size = sizeof(clientname);
          int new = accept(main_sock, (struct sockaddr *) &clientname, &size);
          if (new < 0) {
            perror("accept");
            exit(EXIT_FAILURE);
          }
          
          fd_last_mod_time[new] = time(NULL);
          
          // init buffer
          if (rd_wr_buffers[new] == NULL) {
            rd_wr_buf_new(new);
          } else {
            // TODO: do we need to reset the rd_wr_buffer here? Yes, but why?
            rd_wr_buf_reset(rd_wr_buffers[new]);
            write_buffer_lookup[new] = NULL;
          }
        }
        continue;
      }
      
      // read to the buffer
      if (FD_ISSET(i, &read_fd_set)) {
        
        int cache_index = rd_wr_buffers[i]->fd_id - FD_SETSIZE;
        fd_last_mod_time[i] = time(NULL);
        
        if (cache_index >= 0) {
          // assume our cache will never be full
          num_bytes = read(i, rd_wr_buffers[i]->buffer + rd_wr_buffers[i]->size,
                               RD_WR_BUFFER_SIZE);
          
          if (num_bytes >= 1) {
            cache_ele *the_cache = cache_arr[cache_index];
            response_from_server* response = the_cache->value;
            response->msg_len += num_bytes;
            
            rd_wr_buffers[i]->size += num_bytes;
            
            // try to parse the response header
            if (response->body_len == 0) {
              response_parse(the_cache, response->msg_len);
            }
          }
        } else {
          num_bytes = read(i, rd_wr_buffers[i]->buffer + rd_wr_buffers[i]->size,
                               RD_WR_BUFFER_SIZE - rd_wr_buffers[i]->size);
          
          if (num_bytes >= 1) {
            rd_wr_buffers[i]->size += num_bytes;
            
            // only process when it is the 1st request from client
            if (write_buffer_lookup[i] == NULL) {
              // validate, parse and send request/reply, find its write_buf
              process_client_request(i);
            }
          }
        }
        
        if (num_bytes < 1) {
          fd_last_mod_time[i] = 0;
          
          // shut fd and close the connection
          shutdown(i, SHUT_RDWR);
          close(i);
		  FD_CLR(i, &read_fd_set);
		  continue;
        }
      }
      
      // write to the buffer or cache
      if (FD_ISSET(i, &write_fd_set)) {
        
        rd_wr_buffer *write_buf = write_buffer_lookup[i];
        
        num_bytes = write_buf->size - write_buf->size_written;
        num_bytes = RD_WR_BUFFER_SIZE >= num_bytes ? num_bytes : RD_WR_BUFFER_SIZE; // if from cache
        
        write_buf->size_written += num_bytes;
        fd_last_mod_time[i] = time(NULL);
        
        // check if write buffer is full
        if (write_buf->fd_id < FD_SETSIZE && write_buf->size_written == RD_WR_BUFFER_SIZE) {
          write_buf->size_written = write_buf->size = 0;
        }
      }
    }
  }
}

void init() {
  rd_wr_buf_init();
  cache_init();
  fd_last_mod_time = (long *) calloc(FD_SETSIZE, sizeof(long));
}

int connect_server(request_from_client *req) {
  struct hostent *server;
  int sock_fd;
  struct sockaddr_in server_addr;
  
  // socket: create the socket
  sock_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (sock_fd < 0) {
    perror("ERROR opening socket\n");
    exit(EXIT_FAILURE);
  }
  
  // gethostbyname: get the server's DNS entry
  server = gethostbyname(req->host_name);
  if (server == NULL) {
    perror("ERROR getting hostname.\n");
    exit(EXIT_FAILURE);
  }
  
  // build the server's Internet address
  bzero((char *) &server_addr, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  bcopy((char *)server->h_addr_list[0], (char *)&server_addr.sin_addr.s_addr, server->h_length);
  server_addr.sin_port = htons(req->port_num);
  
  // connect: create a connection with the server
  if (connect(sock_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
    perror("ERROR connecting\n");
    exit(EXIT_FAILURE);      
  }
  
  return sock_fd;
}

void process_client_request(int fd_id) {
  rd_wr_buffer *read_buf = rd_wr_buffers[fd_id];
  
  // parse the request
  request_from_client *req = request_parse(read_buf);
  
  if (req != NULL) {
    // if it is a GET request
    if (req->is_get_request == true) {
      write_buffer_lookup[fd_id] = process_get_request(req, read_buf);
    } else {
      write_buffer_lookup[fd_id] = process_connect_request(req, read_buf);
    }
  }
}

rd_wr_buffer* process_get_request(request_from_client *req, rd_wr_buffer* write_buf) {
  // find the GET request in the cache first
  cache_ele* res_cache = cache_find(req->request_method);
  rd_wr_buffer *res_buf = (rd_wr_buffer *) malloc(sizeof(rd_wr_buffer));
  long cur_time = time(NULL);
  
  // if cache value not exist or out-dated
  if (res_cache == NULL
      || res_cache->expiration_time <= cur_time) {
    // create an empty content cache
    res_cache = cache_new_or_reset(res_cache, req);
    
    // create fd for server
    int server_fd = connect_server(req);
    rd_wr_buffers[server_fd] = res_buf;
    write_buffer_lookup[server_fd] = write_buf;
    fd_last_mod_time[server_fd] = cur_time;
  }
  
  res_buf->fd_id = FD_SETSIZE + res_cache->id;
  res_buf->buffer = res_cache->value->msg;
  res_buf->size = res_cache->value->msg_len;
  res_buf->size_written = 0;
  
  return res_buf;
}

rd_wr_buffer* process_connect_request(request_from_client *req, rd_wr_buffer* write_buf) {  
  // create fd for server
  int server_fd = connect_server(req);
  fd_last_mod_time[server_fd] = time(NULL);
  write_buffer_lookup[server_fd] = write_buf;
  
  if (rd_wr_buffers[server_fd] == NULL) {
    rd_wr_buf_new(server_fd);
  }
  rd_wr_buf_reset(rd_wr_buffers[server_fd]);
  
  // reply to client
  char reply[] = "HTTP/1.1 200 Connection Established\r\nProxy-Agent: MyProxy/0.1\r\n\r\n";
  memcpy(rd_wr_buffers[server_fd]->buffer, reply, strlen(reply));
  rd_wr_buffers[server_fd]->size = strlen(reply);
  
  // reset write_buf (client read buffer)
  write_buf->size = 0;
  write_buf->size_written = 0;
  
  return rd_wr_buffers[server_fd];
}

int make_socket(uint16_t port) {
  int sock;
  struct sockaddr_in name;

  /* Create the socket. */
  sock = socket(PF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    perror("socket");
    exit(EXIT_FAILURE);
  }

  /* Give the socket a name. */
  name.sin_family = AF_INET;
  name.sin_port = htons(port);
  name.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(sock, (struct sockaddr *) &name, sizeof(name)) < 0) {
    perror("bind");
    exit(EXIT_FAILURE);
  }

  return sock;
}

void rd_wr_buf_init() {
  rd_wr_buffers = (rd_wr_buffer **) calloc(FD_SETSIZE, sizeof(rd_wr_buffer*));
  write_buffer_lookup = (rd_wr_buffer **) calloc(FD_SETSIZE, sizeof(rd_wr_buffer*));
}

void cache_init() {
  cache_arr = (cache_ele **) malloc(CACHE_CAP * sizeof(cache_ele*));
  for (int i = 0; i < CACHE_CAP; i++) {
    cache_arr[i] = cache_new();
    cache_arr[i]->id = i;
  }
}

cache_ele* cache_new() {
  cache_ele *res = (cache_ele *) malloc(sizeof(cache_ele));
  res->value = response_new();
  cache_reset(res);
  return res;
}

void cache_reset(cache_ele *cache) {
  cache->key[0] = '\0';
  cache->expiration_time = 0;
  response_reset(cache->value);
}

response_from_server* response_new() {
  response_from_server *res = (response_from_server *) malloc(sizeof(response_from_server));
  res->msg = (char *) malloc(RESPONSE_MSG_SIZE * sizeof(char));
  return res;
}

void response_reset(response_from_server *response) {
  bzero(response->msg, RESPONSE_MSG_SIZE);
  response->body = NULL;
  response->body_len = 0;
  response->msg_len = 0;
}

void rd_wr_buf_new(int fd_id) {
  rd_wr_buffers[fd_id] = (rd_wr_buffer *) malloc(sizeof(rd_wr_buffer));
  rd_wr_buffers[fd_id]->buffer = (char *) malloc(RD_WR_BUFFER_SIZE * sizeof(char));
  rd_wr_buffers[fd_id]->fd_id = fd_id;
  rd_wr_buf_reset(rd_wr_buffers[fd_id]);
}

void rd_wr_buf_reset(rd_wr_buffer *buf) {
  buf->size = 0;
  buf->size_written = 0;
  //bzero(buf->buffer, RD_WR_BUFFER_SIZE);
}

bool is_request_end(char *str, int len) {
  if (str[len - 1] == '\n' && str[len - 2] == '\r' && str[len - 3] == '\n'
      && str[len - 4] == '\r') {
    return true;
  }
  return false;
}

int parse_port_number(char *str) {
  // parse port num if available
  if (str[0] == ':' && str[1] >= '0' && str[1] <= '9') {
    
    char port_chars[10];
    int i = 0;
    
    while (str[i + 1] >= '0' && str[i + 1] <= '9') {
      port_chars[i] = str[i + 1];
      i++;
    }
    port_chars[i] = '\0';
    
    return atoi(port_chars);
  }
  
  return 0;
}

request_from_client* request_parse(rd_wr_buffer *req_buf) {
  
  // if it is a valid request, return a new request_from_client
  request_from_client *request = (request_from_client*) malloc(sizeof(request_from_client));
  request->host_name[0] = '\0';
  request->request_method[0] = '\0';
  request->port_num = 80;
  memcpy(request->msg, req_buf->buffer, req_buf->size);
  request->msg[req_buf->size] = '\0';
  request->is_get_request = true;
  
  int get_len = 4;
  int host_len = 6;
  int connect_len = 8;
  char connect_header[8] = "CONNECT ";
  char get_header[4] = "GET ";
  char host_header[6] = "Host: ";
  
  for (int i = 0; i < req_buf->size; i++) {
    char *line = req_buf->buffer + i;
    if (contains_chars(line, get_header, get_len)
        || contains_chars(line, connect_header, connect_len)) {
      
      int j = 0;
      while (req_buf->buffer[i] != '\r') {
        int port_num_parsed = parse_port_number(line + j);
        if (port_num_parsed > 0){
          request->port_num = port_num_parsed;
        }
        
        request->request_method[j++] = req_buf->buffer[i++];
      }
      i++; // now is '\n'
      request->request_method[j] = '\0';
      printf("HTTP request method is:[%s]\n", request->request_method);
      
      if (request->request_method[0] == 'C') {
        request->is_get_request = false;
      }
    } else if (contains_chars(line, host_header, host_len)) {
      
      i += host_len;
      int j = 0;
      while (req_buf->buffer[i] != '\r' && req_buf->buffer[i] != ':') {
        request->host_name[j++] = req_buf->buffer[i++];
      }
      request->host_name[j] = '\0';
      
      while (req_buf->buffer[i] != '\n') {
        i++;
      }
      // now is '\n'
    } else {
      // neglect this line
      while (req_buf->buffer[i] != '\n') {
        i++;
      }
    }
  }
  
  // otherwise, return NULL
  if (strlen(request->host_name) == 0 || strlen(request->request_method) == 0) {
    free(request);
    return NULL;
  } else {
    return request;
  }
}

bool contains_chars(char *ptr, char *header, int len) {
  for (int i = 0; i < len; i++) {
    if (ptr[i] != header[i]) {
      return false;
    }
  }
  
  return true;
}

cache_ele* cache_find(char *key) {
  for (int i = 0; i < CACHE_CAP; i++) {
    if (strcmp(key, cache_arr[i]->key) == 0) {
      return cache_arr[i];
    }
  }
  return NULL;
}

cache_ele* cache_next_free() {
  int res = CACHE_CAP; // earliest expir index
  for (int i = 0; i < CACHE_CAP; i++) {
    if (strlen(cache_arr[i]->key) == 0) {
      return cache_arr[i];
    } else if (res == CACHE_CAP
               || cache_arr[i]->expiration_time < cache_arr[res]->expiration_time) {
      res = i;
    }
  }
  
  cache_reset(cache_arr[res]);
  return cache_arr[res];
}

cache_ele* cache_new_or_reset(cache_ele* node, request_from_client *req) {
  // create a new cache node if NULL
  if (node == NULL) {
    node = cache_next_free();
    strcpy(node->key, req->request_method);
  }
  
  // reset the content of cache
  
  // return the node
  return node;
}

void response_parse(cache_ele *node, int len) {
  int len_cache_ctrl = 15;
  int len_content = 16;
  int len_max_age = 8;
  char cache_ctrl[15] = "Cache-Control: ";
  char content_len[16] = "Content-Length: ";
  char max_age[8] = "max-age=";
  char *res_buf = node->value->msg;
  long cur_time = time(NULL);
  
  long parsed_expir_time = cur_time + DEFAULT_CACHE_AGE;;
  long parsed_body_len = 0;

  
  for (int i = 0; i < len; i++) {
    char *line = res_buf + i;
    
    // the separting line between header and body '\r\n'
    if (res_buf[i] == '\r' && node->value->body == NULL) {
      
      i++; // '\n'
      node->value->body = res_buf + i + 1;
      
      // only to parse the info in the header and only assign values when reached this blank line
      node->value->body_len = parsed_body_len;
      node->expiration_time = parsed_expir_time;
      break;
    // get content-length
    } else if (contains_chars(line, content_len, len_content)) {
      
      char tmp_len[100];
      int j = 0;
      i += len_content;
      while (res_buf[i] != '\r') {
        tmp_len[j++] = res_buf[i++];
      }
      tmp_len[j] = '\0';
      i++; // '\n'
      
      //node->value->body_len = atol(tmp_len);
      parsed_body_len = atol(tmp_len);
    // get max-age in Cache-Control
    } else if (contains_chars(line, cache_ctrl, len_cache_ctrl)) {
      
      i += len_cache_ctrl;
      while (res_buf[i] != '\r') {
        if (contains_chars(res_buf + i, max_age, len_max_age)) {
          i += len_max_age;
          char tmp_age[100];
          int j = 0;
          while (res_buf[i] <= '9' && res_buf[i] >= '0') {
            tmp_age[j++] = res_buf[i++];
          }
          tmp_age[j] = '\0';
          //node->expiration_time = cur_time + atol(tmp_age);
          parsed_expir_time = cur_time + atol(tmp_age);
        } else {
          i++;
        }
      }
      i++; // '\n'
    // neglect this line  
    } else {
      while (res_buf[i] != '\n') {
        i++;
      }
    }
  }
}
