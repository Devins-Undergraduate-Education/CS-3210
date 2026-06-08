#include "types.h"
#include "user.h"
#include "login.h"
#include "crypto.h"
#include "fcntl.h"

#define USER_DB_PATH "/login.db"
#define ROOT_USERNAME "root"
#define ROOT_PASSWORD "admin"
#define MAX_USERNAME_LEN 32
#define SALT_SIZE 16
#define MAX_PASSWORD_LEN (MAX_INPUT_SIZE - 1)
#define MIN_USER_UID 1
#define MAX_USER_UID 0xFFFF

struct user_record {
  ushort uid;
  char username[MAX_USERNAME_LEN];
  uchar salt[SALT_SIZE];
  uchar password_hash[SHA256_SIZE_BYTES];
};

static uchar g_aes_key[AES256_KEY_SIZE_BYTES];
static int g_key_ready = 0;
static int g_db_ready = 0;

static void ensure_crypto_key(void);
static void transform_record(void *buf);
static int read_exact(int fd, void *buf, int len);
static int write_exact(int fd, const void *buf, int len);
static int read_record(int fd, struct user_record *rec);
static int write_record(int fd, const struct user_record *rec);
static int find_user_record(const char *username, struct user_record *out);
static int append_record(const struct user_record *rec);
static void generate_salt(uchar salt[SALT_SIZE]);
static void hash_password(const char *password, const uchar salt[SALT_SIZE],
                          uchar out[SHA256_SIZE_BYTES]);
static int verify_password(const struct user_record *rec,
                           const char *password);
static int validate_username(const char *username);
static int validate_password(const char *password);
static void set_record_username(struct user_record *rec, const char *username);
static int ensure_user_db_ready(void);
static int ensure_root_user(void);

/**
 * Hook into user/src/login/login_init.c in order to intialize any files or
 * data structures necessary for the login system
 * 
 * Called once per boot of xv6
 */
void init_hook() {
  if (ensure_user_db_ready() < 0) {
    printf(2, "login: failed to initialize user database\n");
  }
}

/**
 * Check if user exists in system 
 * 
 * @param username A null-terminated string representing the username
 * @return 0 on success if user exists, -1 for failure otherwise
 */
int does_user_exist(char *username) {
  if (ensure_user_db_ready() < 0)
    return -1;
  if (validate_username(username) < 0)
    return -1;
  return find_user_record(username, 0) == 0 ? 0 : -1;
}

/**
 * Create a user in the system associated with the username and password. Cannot
 * overwrite an existing username with a new password. Expectation is for
 * created users to have a unique non-root uid.
 * 
 * @param username A null-terminated string representing the username
 * @param password A null-terminated string representing the password
 * @return 0 on success, -1 for failure
 */
int create_user(char *username, char *password) {
  if (ensure_user_db_ready() < 0)
    return -1;
  if (validate_username(username) < 0 || validate_password(password) < 0)
    return -1;
  if (strcmp(username, ROOT_USERNAME) == 0)
    return -1;

  int fd = open(USER_DB_PATH, O_RDWR);
  if (fd < 0)
    return -1;

  ushort max_uid = 0;
  struct user_record rec;
  int status;
  while ((status = read_record(fd, &rec)) == 1) {
    if (strcmp(rec.username, username) == 0) {
      close(fd);
      return -1;
    }
    if (rec.uid > max_uid)
      max_uid = rec.uid;
  }
  if (status < 0) {
    close(fd);
    return -1;
  }

  uint next_uid = max_uid >= MIN_USER_UID ? (uint) max_uid + 1 : MIN_USER_UID;
  if (next_uid > MAX_USER_UID) {
    close(fd);
    return -1;
  }

  struct user_record new_rec;
  memset(&new_rec, 0, sizeof(new_rec));
  new_rec.uid = (ushort) next_uid;
  generate_salt(new_rec.salt);
  set_record_username(&new_rec, username);
  hash_password(password, new_rec.salt, new_rec.password_hash);

  if (write_record(fd, &new_rec) < 0) {
    close(fd);
    return -1;
  }

  close(fd);
  return 0;
}

/**
 * Login a user in the system associated with the username and password. Launch
 * the shell under the right permissions for the user. If no such user exists
 * or the password is incorrect, then login will fail.
 * 
 * @param username A null-terminated string representing the username
 * @param password A null-terminated string representing the password
 * @return no return on success, -1 for failure
 */
int login_user(char *username, char *password) {
  if (ensure_user_db_ready() < 0)
    return -1;
  if (validate_username(username) < 0 || validate_password(password) < 0)
    return -1;

  struct user_record rec;
  int lookup = find_user_record(username, &rec);
  if (lookup != 0)
    return -1;

  if (verify_password(&rec, password) < 0)
    return -1;

  if (setuid(rec.uid) < 0)
    return -1;

  char *argv[] = { "sh", 0 };
  if (exec("sh", argv) < 0)
    return -1;

  return -1;
}

static void ensure_crypto_key(void) {
  if (g_key_ready)
    return;

  const char *seed = "gt-cs3210-login-key";
  uchar digest[SHA256_SIZE_BYTES];
  sha256(seed, strlen(seed), digest);
  memmove(g_aes_key, digest, AES256_KEY_SIZE_BYTES);
  memset(digest, 0, sizeof(digest));
  g_key_ready = 1;
}

static void transform_record(void *buf) {
  ensure_crypto_key();
  aes256_encrypt(g_aes_key, buf, sizeof(struct user_record));
}

static int read_exact(int fd, void *buf, int len) {
  int total = 0;
  char *ptr = (char *) buf;
  while (total < len) {
    int n = read(fd, ptr + total, len - total);
    if (n == 0) {
      return total == 0 ? 0 : -1;
    }
    if (n < 0)
      return -1;
    total += n;
  }
  return 1;
}

static int write_exact(int fd, const void *buf, int len) {
  int total = 0;
  const char *ptr = (const char *) buf;
  while (total < len) {
    int n = write(fd, ptr + total, len - total);
    if (n <= 0)
      return -1;
    total += n;
  }
  return 0;
}

static int read_record(int fd, struct user_record *rec) {
  uchar raw[sizeof(struct user_record)];
  int status = read_exact(fd, raw, sizeof(raw));
  if (status <= 0)
    return status;

  transform_record(raw);
  memmove(rec, raw, sizeof(struct user_record));
  memset(raw, 0, sizeof(raw));
  return 1;
}

static int write_record(int fd, const struct user_record *rec) {
  struct user_record tmp;
  memmove(&tmp, rec, sizeof(tmp));
  transform_record(&tmp);
  int rc = write_exact(fd, &tmp, sizeof(tmp));
  memset(&tmp, 0, sizeof(tmp));
  return rc;
}

static int find_user_record(const char *username, struct user_record *out) {
  int fd = open(USER_DB_PATH, O_RDONLY);
  if (fd < 0)
    return -1;

  struct user_record rec;
  int status;
  while ((status = read_record(fd, &rec)) == 1) {
    if (strcmp(rec.username, username) == 0) {
      if (out)
        memmove(out, &rec, sizeof(rec));
      close(fd);
      return 0;
    }
  }

  close(fd);
  if (status < 0)
    return -1;
  return 1;
}

static int append_record(const struct user_record *rec) {
  int fd = open(USER_DB_PATH, O_RDWR);
  if (fd < 0)
    return -1;

  struct user_record tmp;
  int status;
  while ((status = read_record(fd, &tmp)) == 1)
    ;
  if (status < 0) {
    close(fd);
    return -1;
  }

  int rc = write_record(fd, rec);
  close(fd);
  return rc;
}

static void generate_salt(uchar salt[SALT_SIZE]) {
  uint seed = uptime();
  int i;
  seed ^= (uint) getpid() << 16;
  for (i = 0; i < SALT_SIZE; i++) {
    seed = seed * 1664525 + 1013904223 + i;
    salt[i] = (uchar) (seed & 0xFF);
  }
}

static void hash_password(const char *password, const uchar salt[SALT_SIZE],
                          uchar out[SHA256_SIZE_BYTES]) {
  uchar buf[SALT_SIZE + MAX_PASSWORD_LEN];
  memmove(buf, salt, SALT_SIZE);
  uint len = strlen(password);
  if (len > MAX_PASSWORD_LEN)
    len = MAX_PASSWORD_LEN;
  memmove(buf + SALT_SIZE, password, len);
  sha256(buf, SALT_SIZE + len, out);
  memset(buf, 0, sizeof(buf));
}

static int verify_password(const struct user_record *rec,
                           const char *password) {
  uchar candidate[SHA256_SIZE_BYTES];
  int i;
  hash_password(password, rec->salt, candidate);
  int diff = 0;
  for (i = 0; i < SHA256_SIZE_BYTES; i++)
    diff |= (candidate[i] ^ rec->password_hash[i]);
  memset(candidate, 0, sizeof(candidate));
  return diff == 0 ? 0 : -1;
}

static int is_username_char_valid(char c) {
  if (c >= 'a' && c <= 'z') return 1;
  if (c >= 'A' && c <= 'Z') return 1;
  if (c >= '0' && c <= '9') return 1;
  if (c == '_' || c == '-' || c == '.') return 1;
  return 0;
}

static int validate_username(const char *username) {
  uint i;
  if (username == 0)
    return -1;
  uint len = strlen(username);
  if (len == 0 || len >= MAX_USERNAME_LEN)
    return -1;
  for (i = 0; i < len; i++) {
    if (!is_username_char_valid(username[i]))
      return -1;
  }
  return 0;
}

static int validate_password(const char *password) {
  if (password == 0)
    return -1;
  uint len = strlen(password);
  if (len == 0 || len > MAX_PASSWORD_LEN)
    return -1;
  return 0;
}

static void set_record_username(struct user_record *rec, const char *username) {
  uint len = strlen(username);
  if (len >= MAX_USERNAME_LEN)
    len = MAX_USERNAME_LEN - 1;
  memset(rec->username, 0, sizeof(rec->username));
  memmove(rec->username, username, len);
  rec->username[len] = 0;
}

static int ensure_user_db_ready(void) {
  if (g_db_ready)
    return 0;

  int fd = open(USER_DB_PATH, O_RDWR | O_CREATE);
  if (fd < 0)
    return -1;
  close(fd);

  if (chown(USER_DB_PATH, 0) < 0)
    return -1;
  if (chmod(USER_DB_PATH, 0) < 0)
    return -1;

  if (ensure_root_user() < 0)
    return -1;

  g_db_ready = 1;
  return 0;
}

static int ensure_root_user(void) {
  struct user_record rec;
  int status = find_user_record(ROOT_USERNAME, &rec);
  if (status == 0)
    return 0;
  if (status < 0)
    return -1;

  memset(&rec, 0, sizeof(rec));
  rec.uid = 0;
  set_record_username(&rec, ROOT_USERNAME);
  generate_salt(rec.salt);
  hash_password(ROOT_PASSWORD, rec.salt, rec.password_hash);
  return append_record(&rec);
}
