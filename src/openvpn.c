/**
 * collectd - src/openvpn.c
 * Copyright (C) 2008       Doug MacEachern
 * Copyright (C) 2009,2010  Florian octo Forster
 * Copyright (C) 2009       Marco Chiappero
 * Copyright (C) 2009       Fabian Schuh
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; only version 2 of the License is applicable.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Authors:
 *   Doug MacEachern <dougm at hyperic.com>
 *   Florian octo Forster <octo at collectd.org>
 *   Marco Chiappero <marco at absence.it>
 *   Fabian Schuh <mail at xeroc.org>
 *   Pavel Rochnyak <pavel2000 ngs.ru>
 **/

#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"

/**
 * There is two main kinds of OpenVPN status file:
 * - for 'single' mode (point-to-point or client mode)
 * - for 'multi' mode  (server with multiple clients)
 *
 * For 'multi' there is 3 versions of status file format:
 * - version 1 - First version of status file: without line type tokens,
 *   comma delimited for easy machine parsing. Currently used by default.
 *   Added in openvpn-2.0-beta3.
 * - version 2 - with line type tokens, with 'HEADER' line type, uses comma
 *   as a delimiter.
 *   Added in openvpn-2.0-beta15.
 * - version 3 - The only difference from version 2 is delimiter: in version 3
 *   tabs are used instead of commas. Set of fields is the same.
 *   Added in openvpn-2.1_rc14.
 *
 * For versions 2/3 there may be different sets of fields in different
 * OpenVPN versions.
 *
 * Versions 2.0, 2.1, 2.2:
 * Common Name,Real Address,Virtual Address,
 * Bytes Received,Bytes Sent,Connected Since,Connected Since (time_t)
 *
 * Version 2.3:
 * Common Name,Real Address,Virtual Address,
 * Bytes Received,Bytes Sent,Connected Since,Connected Since (time_t),Username
 *
 * Version 2.4:
 * Common Name,Real Address,Virtual Address,Virtual IPv6 Address,
 * Bytes Received,Bytes Sent,Connected Since,Connected Since (time_t),Username,
 * Client ID,Peer ID
 *
 * Current Collectd code tries to handle changes in this field set,
 * if they are backward-compatible.
 **/

#define TITLE_SINGLE "OpenVPN STATISTICS\n"
#define TITLE_V1 "OpenVPN CLIENT LIST\n"
#define TITLE_V2 "TITLE"

#define V1HEADER                                                               \
  "Common Name,Real Address,Bytes Received,Bytes Sent,Connected Since\n"

struct vpn_status_s {
  char *file;
  char *name;
};
typedef struct vpn_status_s vpn_status_t;

static bool new_naming_schema;
static bool collect_compression = true;
static bool collect_user_count;
static bool collect_individual_users = true;

static const char *config_keys[] = {
    "StatusFile",           "Compression", /* old, deprecated name */
    "ImprovedNamingSchema", "CollectCompression",
    "CollectUserCount",     "CollectIndividualUsers"};
static int config_keys_num = STATIC_ARRAY_SIZE(config_keys);

/* Helper function
 * copy-n-pasted from common.c - changed delim to ",\t"  */
static int openvpn_strsplit(char *string, char **fields, size_t size) {
  size_t i = 0;
  char *ptr = string;
  char *saveptr = NULL;

  while ((fields[i] = strtok_r(ptr, ",\t", &saveptr)) != NULL) {
    ptr = NULL;
    i++;

    if (i >= size)
      break;
  }

  return i;
} /* int openvpn_strsplit */

static void openvpn_free(void *arg) {
  vpn_status_t *st = arg;

  sfree(st->file);
  sfree(st);
} /* void openvpn_free */

/* dispatches number of users */
static void numusers_submit(const char *pinst, const char *tinst,
                            gauge_t value) {
  value_list_t vl = VALUE_LIST_INIT;

  vl.values = &(value_t){.gauge = value};
  vl.values_len = 1;
  sstrncpy(vl.plugin, "openvpn", sizeof(vl.plugin));
  sstrncpy(vl.type, "users", sizeof(vl.type));
  if (pinst != NULL)
    sstrncpy(vl.plugin_instance, pinst, sizeof(vl.plugin_instance));
  if (tinst != NULL)
    sstrncpy(vl.type_instance, tinst, sizeof(vl.type_instance));

  plugin_dispatch_values(&vl);
} /* void numusers_submit */

/* dispatches stats about traffic (TCP or UDP) generated by the tunnel
 * per single endpoint */
static void iostats_submit(const char *pinst, const char *tinst, derive_t rx,
                           derive_t tx) {
  value_list_t vl = VALUE_LIST_INIT;
  value_t values[] = {
      {.derive = rx}, {.derive = tx},
  };

  /* NOTE ON THE NEW NAMING SCHEMA:
   *       using plugin_instance to identify each vpn config (and
   *       status) file; using type_instance to identify the endpoint
   *       host when in multimode, traffic or overhead when in single.
   */

  vl.values = values;
  vl.values_len = STATIC_ARRAY_SIZE(values);
  sstrncpy(vl.plugin, "openvpn", sizeof(vl.plugin));
  if (pinst != NULL)
    sstrncpy(vl.plugin_instance, pinst, sizeof(vl.plugin_instance));
  sstrncpy(vl.type, "if_octets", sizeof(vl.type));
  if (tinst != NULL)
    sstrncpy(vl.type_instance, tinst, sizeof(vl.type_instance));

  plugin_dispatch_values(&vl);
} /* void traffic_submit */

/* dispatches stats about data compression shown when in single mode */
static void compression_submit(const char *pinst, const char *tinst,
                               derive_t uncompressed, derive_t compressed) {
  value_list_t vl = VALUE_LIST_INIT;
  value_t values[] = {
      {.derive = uncompressed}, {.derive = compressed},
  };

  vl.values = values;
  vl.values_len = STATIC_ARRAY_SIZE(values);
  sstrncpy(vl.plugin, "openvpn", sizeof(vl.plugin));
  if (pinst != NULL)
    sstrncpy(vl.plugin_instance, pinst, sizeof(vl.plugin_instance));
  sstrncpy(vl.type, "compression", sizeof(vl.type));
  if (tinst != NULL)
    sstrncpy(vl.type_instance, tinst, sizeof(vl.type_instance));

  plugin_dispatch_values(&vl);
} /* void compression_submit */

static int single_read(const char *name, FILE *fh) {
  char buffer[1024];
  char *fields[4];
  const int max_fields = STATIC_ARRAY_SIZE(fields);

  derive_t link_rx = 0, link_tx = 0;
  derive_t tun_rx = 0, tun_tx = 0;
  derive_t pre_compress = 0, post_compress = 0;
  derive_t pre_decompress = 0, post_decompress = 0;

  while (fgets(buffer, sizeof(buffer), fh) != NULL) {
    int fields_num = openvpn_strsplit(buffer, fields, max_fields);

    /* status file is generated by openvpn/sig.c:print_status()
     * http://svn.openvpn.net/projects/openvpn/trunk/openvpn/sig.c
     *
     * The line we're expecting has 2 fields. We ignore all lines
     *  with more or less fields.
     */
    if (fields_num != 2) {
      continue;
    }

    if (strcmp(fields[0], "TUN/TAP read bytes") == 0) {
      /* read from the system and sent over the tunnel */
      tun_tx = atoll(fields[1]);
    } else if (strcmp(fields[0], "TUN/TAP write bytes") == 0) {
      /* read from the tunnel and written in the system */
      tun_rx = atoll(fields[1]);
    } else if (strcmp(fields[0], "TCP/UDP read bytes") == 0) {
      link_rx = atoll(fields[1]);
    } else if (strcmp(fields[0], "TCP/UDP write bytes") == 0) {
      link_tx = atoll(fields[1]);
    } else if (strcmp(fields[0], "pre-compress bytes") == 0) {
      pre_compress = atoll(fields[1]);
    } else if (strcmp(fields[0], "post-compress bytes") == 0) {
      post_compress = atoll(fields[1]);
    } else if (strcmp(fields[0], "pre-decompress bytes") == 0) {
      pre_decompress = atoll(fields[1]);
    } else if (strcmp(fields[0], "post-decompress bytes") == 0) {
      post_decompress = atoll(fields[1]);
    }
  }

  iostats_submit(name, "traffic", link_rx, link_tx);

  /* we need to force this order to avoid negative values with these unsigned */
  derive_t overhead_rx =
      (((link_rx - pre_decompress) + post_decompress) - tun_rx);
  derive_t overhead_tx = (((link_tx - post_compress) + pre_compress) - tun_tx);

  iostats_submit(name, "overhead", overhead_rx, overhead_tx);

  if (collect_compression) {
    compression_submit(name, "data_in", post_decompress, pre_decompress);
    compression_submit(name, "data_out", pre_compress, post_compress);
  }

  return 0;
} /* int single_read */

/* for reading status version 1 */
static int multi1_read(const char *name, FILE *fh) {
  char buffer[1024];
  char *fields[10];
  const int max_fields = STATIC_ARRAY_SIZE(fields);
  long long sum_users = 0;
  bool found_header = false;

  /* read the file until the "ROUTING TABLE" line is found (no more info after)
   */
  while (fgets(buffer, sizeof(buffer), fh) != NULL) {
    if (strcmp(buffer, "ROUTING TABLE\n") == 0)
      break;

    if (strcmp(buffer, V1HEADER) == 0) {
      found_header = true;
      continue;
    }

    /* skip the first lines until the client list section is found */
    if (found_header == false)
      /* we can't start reading data until this string is found */
      continue;

    int fields_num = openvpn_strsplit(buffer, fields, max_fields);
    if (fields_num < 4)
      continue;

    if (collect_user_count)
    /* If so, sum all users, ignore the individuals*/
    {
      sum_users += 1;
    }
    if (collect_individual_users) {
      if (new_naming_schema) {
        iostats_submit(name,              /* vpn instance */
                       fields[0],         /* "Common Name" */
                       atoll(fields[2]),  /* "Bytes Received" */
                       atoll(fields[3])); /* "Bytes Sent" */
      } else {
        iostats_submit(fields[0],         /* "Common Name" */
                       NULL,              /* unused when in multimode */
                       atoll(fields[2]),  /* "Bytes Received" */
                       atoll(fields[3])); /* "Bytes Sent" */
      }
    }
  }

  if (ferror(fh))
    return -1;

  if (found_header == false) {
    NOTICE("openvpn plugin: Unknown file format in instance %s, please "
           "report this as bug. Make sure to include "
           "your status file, so the plugin can "
           "be adapted.",
           name);
    return -1;
  }

  if (collect_user_count)
    numusers_submit(name, name, sum_users);

  return 0;
} /* int multi1_read */

/* for reading status version 2 / version 3
 * status file is generated by openvpn/multi.c:multi_print_status()
 * http://svn.openvpn.net/projects/openvpn/trunk/openvpn/multi.c
 */
static int multi2_read(const char *name, FILE *fh) {
  char buffer[1024];
  /* OpenVPN-2.4 has 11 fields of data + 2 fields for "HEADER" and "CLIENT_LIST"
   * So, set array size to 20 elements, to support future extensions.
   */
  char *fields[20];
  const int max_fields = STATIC_ARRAY_SIZE(fields);
  long long sum_users = 0;

  bool found_header = false;
  int idx_cname = 0;
  int idx_bytes_recv = 0;
  int idx_bytes_sent = 0;
  int columns = 0;

  while (fgets(buffer, sizeof(buffer), fh) != NULL) {
    int fields_num = openvpn_strsplit(buffer, fields, max_fields);

    /* Try to find section header */
    if (found_header == false) {
      if (fields_num < 2)
        continue;
      if (strcmp(fields[0], "HEADER") != 0)
        continue;
      if (strcmp(fields[1], "CLIENT_LIST") != 0)
        continue;

      for (int i = 2; i < fields_num; i++) {
        if (strcmp(fields[i], "Common Name") == 0) {
          idx_cname = i - 1;
        } else if (strcmp(fields[i], "Bytes Received") == 0) {
          idx_bytes_recv = i - 1;
        } else if (strcmp(fields[i], "Bytes Sent") == 0) {
          idx_bytes_sent = i - 1;
        }
      }

      DEBUG("openvpn plugin: found MULTI v2/v3 HEADER. "
            "Column idx: cname: %d, bytes_recv: %d, bytes_sent: %d",
            idx_cname, idx_bytes_recv, idx_bytes_sent);

      if (idx_cname == 0 || idx_bytes_recv == 0 || idx_bytes_sent == 0)
        break;

      /* Data row has 1 field ("HEADER") less than header row */
      columns = fields_num - 1;

      found_header = true;
      continue;
    }

    /* Header already found. Check if the line is the section data.
     * If no match, then section was finished and there is no more data.
     * Empty section is OK too.
     */
    if (fields_num == 0 || strcmp(fields[0], "CLIENT_LIST") != 0)
      break;

    /* Check if the data line fields count matches header line. */
    if (fields_num != columns) {
      ERROR("openvpn plugin: File format error in instance %s: Fields count "
            "mismatch.",
            name);
      return -1;
    }

    DEBUG("openvpn plugin: found MULTI v2/v3 CLIENT_LIST. "
          "Columns: cname: %s, bytes_recv: %s, bytes_sent: %s",
          fields[idx_cname], fields[idx_bytes_recv], fields[idx_bytes_sent]);

    if (collect_user_count)
      sum_users += 1;

    if (collect_individual_users) {
      if (new_naming_schema) {
        /* plugin inst = file name, type inst = fields[1] */
        iostats_submit(name,                           /* vpn instance     */
                       fields[idx_cname],              /* "Common Name"    */
                       atoll(fields[idx_bytes_recv]),  /* "Bytes Received" */
                       atoll(fields[idx_bytes_sent])); /* "Bytes Sent"     */
      } else {
        /* plugin inst = fields[idx_cname], type inst = "" */
        iostats_submit(fields[idx_cname], /*              "Common Name"    */
                       NULL,              /* unused when in multimode      */
                       atoll(fields[idx_bytes_recv]),  /* "Bytes Received" */
                       atoll(fields[idx_bytes_sent])); /* "Bytes Sent"     */
      }
    }
  }

  if (ferror(fh))
    return -1;

  if (found_header == false) {
    NOTICE("openvpn plugin: Unknown file format in instance %s, please "
           "report this as bug. Make sure to include "
           "your status file, so the plugin can "
           "be adapted.",
           name);
    return -1;
  }

  if (collect_user_count) {
    numusers_submit(name, name, sum_users);
  }

  return 0;
} /* int multi2_read */

/* read callback */
static int openvpn_read(user_data_t *user_data) {
  char buffer[1024];
  int read = 0;

  vpn_status_t *st = user_data->data;

  FILE *fh = fopen(st->file, "r");
  if (fh == NULL) {
    WARNING("openvpn plugin: fopen(%s) failed: %s", st->file, STRERRNO);

    return -1;
  }

  // Try to detect file format by its first line
  if ((fgets(buffer, sizeof(buffer), fh)) == NULL) {
    WARNING("openvpn plugin: failed to get data from: %s", st->file);
    fclose(fh);
    return -1;
  }

  if (strcmp(buffer, TITLE_SINGLE) == 0) { // OpenVPN STATISTICS
    DEBUG("openvpn plugin: found status file SINGLE");
    read = single_read(st->name, fh);
  } else if (strcmp(buffer, TITLE_V1) == 0) { // OpenVPN CLIENT LIST
    DEBUG("openvpn plugin: found status file MULTI version 1");
    read = multi1_read(st->name, fh);
  } else if (strncmp(buffer, TITLE_V2, strlen(TITLE_V2)) == 0) { // TITLE
    DEBUG("openvpn plugin: found status file MULTI version 2/3");
    read = multi2_read(st->name, fh);
  } else {
    NOTICE("openvpn plugin: %s: Unknown file format, please "
           "report this as bug. Make sure to include "
           "your status file, so the plugin can "
           "be adapted.",
           st->file);
    read = -1;
  }
  fclose(fh);
  return read;
} /* int openvpn_read */

static int openvpn_config(const char *key, const char *value) {
  if (strcasecmp("StatusFile", key) == 0) {
    char callback_name[3 * DATA_MAX_NAME_LEN];
    char *status_name;

    char *status_file = strdup(value);
    if (status_file == NULL) {
      ERROR("openvpn plugin: strdup failed: %s", STRERRNO);
      return 1;
    }

    /* it determines the file name as string starting at location filename + 1
     */
    char *filename = strrchr(status_file, (int)'/');
    if (filename == NULL) {
      /* status_file is already the file name only */
      status_name = status_file;
    } else {
      /* doesn't waste memory, uses status_file starting at filename + 1 */
      status_name = filename + 1;
    }

    /* create a new vpn element */
    vpn_status_t *instance = calloc(1, sizeof(*instance));
    if (instance == NULL) {
      ERROR("openvpn plugin: malloc failed: %s", STRERRNO);
      sfree(status_file);
      return 1;
    }
    instance->file = status_file;
    instance->name = status_name;

    snprintf(callback_name, sizeof(callback_name), "openvpn/%s", status_name);

    int status = plugin_register_complex_read(
        /* group = */ "openvpn",
        /* name      = */ callback_name,
        /* callback  = */ openvpn_read,
        /* interval  = */ 0,
        &(user_data_t){
            .data = instance, .free_func = openvpn_free,
        });

    if (status == EINVAL) {
      WARNING("openvpn plugin: status filename \"%s\" "
              "already used, please choose a "
              "different one.",
              status_name);
      return -1;
    }

    DEBUG("openvpn plugin: status file \"%s\" added", instance->file);
  } /* if (strcasecmp ("StatusFile", key) == 0) */
  else if ((strcasecmp("CollectCompression", key) == 0) ||
           (strcasecmp("Compression", key) == 0)) /* old, deprecated name */
  {
    if (IS_FALSE(value))
      collect_compression = false;
    else
      collect_compression = true;
  } /* if (strcasecmp ("CollectCompression", key) == 0) */
  else if (strcasecmp("ImprovedNamingSchema", key) == 0) {
    if (IS_TRUE(value)) {
      DEBUG("openvpn plugin: using the new naming schema");
      new_naming_schema = true;
    } else {
      new_naming_schema = false;
    }
  } /* if (strcasecmp ("ImprovedNamingSchema", key) == 0) */
  else if (strcasecmp("CollectUserCount", key) == 0) {
    if (IS_TRUE(value))
      collect_user_count = true;
    else
      collect_user_count = false;
  } /* if (strcasecmp("CollectUserCount", key) == 0) */
  else if (strcasecmp("CollectIndividualUsers", key) == 0) {
    if (IS_FALSE(value))
      collect_individual_users = false;
    else
      collect_individual_users = true;
  } /* if (strcasecmp("CollectIndividualUsers", key) == 0) */
  else {
    return -1;
  }

  return 0;
} /* int openvpn_config */

static int openvpn_init(void) {
  if (!collect_individual_users && !collect_compression &&
      !collect_user_count) {
    WARNING("OpenVPN plugin: Neither `CollectIndividualUsers', "
            "`CollectCompression', nor `CollectUserCount' is true. There's no "
            "data left to collect.");
    return -1;
  }

  return 0;
} /* int openvpn_init */

void module_register(void) {
  plugin_register_config("openvpn", openvpn_config, config_keys,
                         config_keys_num);
  plugin_register_init("openvpn", openvpn_init);
} /* void module_register */
