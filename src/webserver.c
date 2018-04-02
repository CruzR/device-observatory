#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <ctype.h>
#include <arpa/inet.h>

#include <microhttpd.h>

#include "data.h"
#include "utils.h"
#include "main.h"
#include "webserver.h"


static struct MHD_Daemon *g_webserver;
static const char *g_webserver_path;

static const char *error_404 = "<html><head><title>Error 404</title></head><body>Error 404</body></html>";


static uint8_t *read_file(size_t *size, const char path[])
{
  uint8_t *fdata;
  long fsize;
  FILE *fp;

  fp = fopen(path, "rb");
  if (NULL == fp)
    return NULL;

  fseek(fp, 0, SEEK_END);
  fsize = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  fdata = malloc(fsize);
  fread(fdata, fsize, 1, fp);
  fclose(fp);

  *size = fsize;

  return fdata;
}

static int is_suffix(const char path[], const char prefix[])
{
  int pathlen = strlen(path);
  int prefixlen = strlen(prefix);

  if (prefixlen >= pathlen) {
    return 0;
  }

  return (0 == memcmp(path + (pathlen - prefixlen), prefix, prefixlen));
}

/*
 * Check if the path consist of "[a-zA-Z0-9.\_-]*".
 * But does not contain "..".
 */
static int is_valid_path(const char path[])
{
  char prev;
  char c;
  int i;

  prev = '\0';
  for (i = 0; path[i]; i++) {
    c = path[i];
    if (!isalnum(c)
        && c != '/' && c != '.'
        && c != '-' && c != '_') {
      return 0;
    }
    if (prev == '.' && c == '.') {
      return 0;
    }
    prev = c;
  }

  return 1;
}

struct mimetype {
  const char *suffix;
  const char *mimetype;
};

struct mimetype g_mimetypes[] = {
  {".html", "text/html; charset=utf-8"},
  {".json", "application/json"},
  {".js", "text/javascript"},
  {".css", "text/css"},
  {".png", "image/png"},
  {".jpg", "image/jpeg"},
  {".jpeg", "image/jpeg"},
  {NULL, NULL}
};

const char *get_mimetype(const char str[])
{
  struct mimetype *mimetype;

  mimetype = &g_mimetypes[0];
  while (mimetype->suffix) {
    if (is_suffix(str, mimetype->suffix)) {
      return mimetype->mimetype;
    }
    mimetype++;
  }

  return "application/octet-stream";
}

static int send_response(void *cls, struct MHD_Connection *connection,
  const char *url, const char *method, const char *version,
  const char *upload_data, size_t *upload_data_size, void **con_cls)
{
  const union MHD_ConnectionInfo *connection_info;
  struct MHD_Response *response;
  char content_path[256];
  uint8_t *content_data;
  size_t content_size;
  struct device *device;
  int is_localhost;
  FILE *fp;
  int ret;

  connection_info = MHD_get_connection_info(connection, MHD_CONNECTION_INFO_CLIENT_ADDRESS);
  if (!connection_info) {
    goto error;
  }

  debug("connection from IP address: %s\n", str_addr(connection_info->client_addr));

  if (0 == strcmp(url, "/device-observatory.json")) {
    is_localhost = addr_is_localhost(
      connection_info->client_addr
    );

    device = find_device_by_ip(
      connection_info->client_addr
    );

    content_data = NULL;
    content_size = 0;
    if (device || is_localhost) {
      fp = open_memstream((char**) &content_data, &content_size);
      if (device) {
        // get only own device info
        write_device_json(fp, device);
      } else {
        // get all device info for localhost access
        write_devices_json(fp);
      }
      fclose(fp);
    } else {
      goto error;
    }

    response = MHD_create_response_from_buffer(content_size, content_data, MHD_RESPMEM_MUST_FREE);
    MHD_add_response_header(response, "Content-Type", "application/json");
    ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
  } else {
    if (0 == strcmp(url, "/")) {
      url = "/index.html";
    }

    snprintf(content_path, sizeof(content_path), "%s/%s", g_webserver_path, url);
    if (!is_valid_path(content_path)) {
      goto error;
    }

    content_data = read_file(&content_size, content_path);

    if (NULL == content_data) {
      goto error;
    }

  	response = MHD_create_response_from_buffer(content_size, content_data, MHD_RESPMEM_MUST_FREE);
    MHD_add_response_header(response, "Content-Type", get_mimetype(url));
    ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
  }

  MHD_destroy_response(response);

  return ret;

error:
  response = MHD_create_response_from_buffer(strlen(error_404), (char *)error_404, MHD_RESPMEM_PERSISTENT);
  MHD_add_response_header(response, "Content-Type", "text/html; charset=utf-8");
  ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response);
  MHD_destroy_response(response);
  return ret;
}

int webserver_start(const char path[], int port)
{
  char pathbuf[256];
  char *p;

  p = realpath(path, pathbuf);
  if (NULL == p) {
    return EXIT_FAILURE;
  }

  g_webserver_path = strdup(p);
  g_webserver = MHD_start_daemon(0, port, NULL, NULL, &send_response, NULL, MHD_OPTION_END);

  if (g_webserver) {
    return EXIT_SUCCESS;
  } else {
    fprintf(stderr, "Failed to create webserver: %s\n", strerror(errno));
    return EXIT_FAILURE;
  }
}

void webserver_before_select(fd_set *rs, fd_set *ws, fd_set *es, int *max_fd)
{
  if (MHD_YES != MHD_get_fdset(g_webserver, rs, ws, es, max_fd)) {
    fprintf(stderr, "MHD_get_fdset(): %s", strerror(errno));
    exit(1);
  }
}

void webserver_after_select()
{
  MHD_run(g_webserver);
}