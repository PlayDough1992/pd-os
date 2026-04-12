/* ============================================================================
 * PD-Kernel  —  User account system
 *
 * Hardcoded user table with FNV-1a 32-bit password hashing.
 * Passwords are stored only as hashes after users_init() runs.
 * When a filesystem is added, migrate this table to /etc/passwd.
 * ============================================================================ */

#include "users.h"

/* ---- FNV-1a 32-bit constants ---------------------------------------------- */

#define FNV_OFFSET  2166136261u
#define FNV_PRIME   16777619u

/* ---- Static helpers ------------------------------------------------------- */

static int u_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void u_memset(void *ptr, uint8_t val, int n)
{
    uint8_t *b = (uint8_t *)ptr;
    while (n--) *b++ = val;
}

/* ---- User table ----------------------------------------------------------- */
/*
 * init_passwords holds plaintext passwords ONLY until users_init() runs.
 * users_init() hashes each entry into user_table then zeroes this array.
 * Add new users here: keep user_table and init_passwords in sync (same order).
 */

static char init_passwords[MAX_USERS][USERNAME_LEN] = {
    "root",   /* uid 0 */
    "pd",     /* uid 1 */
};

static user_t user_table[MAX_USERS] = {
    { "root", 0, 0, USER_FLAG_ROOT },
    { "pd",   0, 1, 0              },
    { "",     0, 0, 0              },   /* sentinel — always last */
};

/* ---- Hash ----------------------------------------------------------------- */

uint32_t users_hash(const char *s)
{
    uint32_t hash = FNV_OFFSET;
    while (*s) {
        hash ^= (uint8_t)*s++;
        hash *= FNV_PRIME;
    }
    return hash;
}

/* ---- Init ----------------------------------------------------------------- */

void users_init(void)
{
    int i;
    for (i = 0; i < MAX_USERS && user_table[i].username[0]; i++) {
        user_table[i].password_hash = users_hash(init_passwords[i]);
        /* Overwrite plaintext password with zeroes */
        u_memset(init_passwords[i], 0, u_strlen(init_passwords[i]));
    }
}

/* ---- Lookup --------------------------------------------------------------- */

static int u_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (*a == '\0' && *b == '\0');
}

const user_t *users_get(const char *username)
{
    int i;
    for (i = 0; i < MAX_USERS && user_table[i].username[0]; i++) {
        if (u_streq(user_table[i].username, username))
            return &user_table[i];
    }
    return NULL;
}

int users_verify(const char *username, const char *password)
{
    const user_t *u = users_get(username);
    if (!u) return 0;
    return users_hash(password) == u->password_hash;
}
