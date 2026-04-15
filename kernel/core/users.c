/* ============================================================================
 * PD-Kernel  —  User account system
 *
 * Hardcoded user table with FNV-1a 32-bit password hashing.
 * Passwords are stored only as hashes after users_init() runs.
 * When a filesystem is added, migrate this table to /etc/passwd.
 * ============================================================================ */

#include "users.h"
#include "vfs.h"
#include "pdfs.h"

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

int users_count(void)
{
    int i;
    for (i = 0; i < MAX_USERS && user_table[i].username[0]; i++)
        ;
    return i;
}

const user_t *users_get_by_index(int i)
{
    if (i < 0 || i >= MAX_USERS || !user_table[i].username[0])
        return (void *)0;
    return &user_table[i];
}

/* ---- User management ----------------------------------------------------- */

int users_add(const char *username, uint8_t is_admin, const char *password)
{
    int i, last, ulen;
    uint8_t max_uid;

    ulen = u_strlen(username);
    if (ulen == 0 || ulen >= USERNAME_LEN) return -2;

    /* Duplicate check */
    if (users_get(username)) return -3;

    /* Find sentinel (first empty slot) */
    for (last = 0; last < MAX_USERS && user_table[last].username[0]; last++);
    if (last >= MAX_USERS - 1) return -1;   /* full — keep at least one sentinel */

    /* Assign next uid = max existing uid + 1 */
    max_uid = 0u;
    for (i = 0; i < last; i++)
        if (user_table[i].uid > max_uid) max_uid = user_table[i].uid;

    /* Populate the slot */
    u_memset(user_table[last].username, 0, USERNAME_LEN);
    for (i = 0; i < ulen; i++) user_table[last].username[i] = username[i];
    user_table[last].username[ulen] = '\0';
    user_table[last].password_hash  = users_hash(password);
    user_table[last].uid            = (uint8_t)(max_uid + 1u);
    user_table[last].flags          = is_admin ? USER_FLAG_ROOT : 0u;

    return 0;
}

int users_remove(const char *username)
{
    const user_t *u;
    int i, found, count;

    /* Refuse to remove root (uid 0) */
    u = users_get(username);
    if (!u)         return -1;
    if (u->uid == 0u) return -2;

    /* Find index and total active count */
    found = -1; count = 0;
    for (i = 0; i < MAX_USERS && user_table[i].username[0]; i++) {
        if (u_streq(user_table[i].username, username)) found = i;
        count++;
    }
    if (found < 0) return -1;

    /* Shift subsequent entries left to close the gap */
    for (i = found; i < count - 1; i++)
        user_table[i] = user_table[i + 1];

    /* Zero the vacated last slot — becomes the new sentinel */
    u_memset(&user_table[count - 1], 0, sizeof(user_t));

    return 0;
}

int users_change_password(const char *username, const char *new_password)
{
    int i;
    for (i = 0; i < MAX_USERS && user_table[i].username[0]; i++) {
        if (u_streq(user_table[i].username, username)) {
            user_table[i].password_hash = users_hash(new_password);
            return 0;
        }
    }
    return -1;
}

/* ---- Disk persistence helpers -------------------------------------------- */

/*
 * On-disk credential record.  Exactly 40 bytes.
 * Stored in /usr/<uid>.pduc, owned by root, mode rw-------.
 */
typedef struct {
    char     username[USERNAME_LEN]; /* 32 bytes: null-terminated            */
    uint32_t password_hash;          /*  4 bytes: FNV-1a hash                */
    uint8_t  uid;                    /*  1 byte : user id                    */
    uint8_t  flags;                  /*  1 byte : USER_FLAG_ROOT etc         */
    uint8_t  pad[2];                 /*  2 bytes: alignment pad              */
} __attribute__((packed)) pduc_t;   /* total: 40 bytes                       */

/* Convert a uint8 to a decimal string (max 3 chars + NUL). */
static void uid_to_str(uint8_t uid, char *buf)
{
    char tmp[4];
    int  n = 0;
    if (uid == 0u) { buf[0]='0'; buf[1]='\0'; return; }
    uint8_t v = uid;
    while (v) { tmp[n++] = (char)('0' + v % 10u); v /= 10u; }
    int i;
    for (i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = '\0';
}

/* Return 1 if name ends with ".pduc". */
static int has_pduc_suffix(const char *name)
{
    int len = 0;
    while (name[len]) len++;
    if (len < 5) return 0;
    return name[len-5]=='.' && name[len-4]=='p' &&
           name[len-3]=='d' && name[len-2]=='u' && name[len-1]=='c';
}

/* Update the password hash of an already-existing table entry by name. */
static void update_hash_by_name(const char *username, uint32_t hash)
{
    int i;
    for (i = 0; i < MAX_USERS && user_table[i].username[0]; i++) {
        if (u_streq(user_table[i].username, username)) {
            user_table[i].password_hash = hash;
            return;
        }
    }
}

/*
 * Add a user directly from a pduc record (no re-hashing, preserves uid).
 * Skips if that username or uid is already in the table.
 */
static int add_raw(const char *username, uint32_t hash, uint8_t uid, uint8_t flags)
{
    int i, last, ulen;
    ulen = u_strlen(username);
    if (ulen == 0 || ulen >= USERNAME_LEN) return -2;
    for (i = 0; i < MAX_USERS && user_table[i].username[0]; i++) {
        if (u_streq(user_table[i].username, username)) return -3;
        if (user_table[i].uid == uid)              return -4;
    }
    last = i;
    if (last >= MAX_USERS - 1) return -1;
    u_memset(user_table[last].username, 0, USERNAME_LEN);
    for (i = 0; i < ulen; i++) user_table[last].username[i] = username[i];
    user_table[last].username[ulen] = '\0';
    user_table[last].password_hash  = hash;
    user_table[last].uid            = uid;
    user_table[last].flags          = flags;
    return 0;
}

/* ---- Public persistence API ---------------------------------------------- */

int users_save_to_disk(const char *username)
{
    const user_t *u;
    pduc_t        pd;
    vfs_node_t    node;
    char          path[16]; /* "/usr/" + 3 digits + ".pduc" + NUL = 14 max  */
    int           i, pi, cr;
    char          uid_str[4];

    u = users_get(username);
    if (!u) return -1;

    /* Build "/usr/<uid>.pduc" */
    uid_to_str(u->uid, uid_str);
    path[0]='/'; path[1]='u'; path[2]='s'; path[3]='r'; path[4]='/';
    pi = 5;
    for (i = 0; uid_str[i]; i++) path[pi++] = uid_str[i];
    path[pi++]='.'; path[pi++]='p'; path[pi++]='d';
    path[pi++]='u'; path[pi++]='c'; path[pi] = '\0';

    /* Fill the credential record */
    u_memset(&pd, 0, (int)sizeof(pduc_t));
    for (i = 0; i < USERNAME_LEN && username[i]; i++) pd.username[i] = username[i];
    pd.password_hash = u->password_hash;
    pd.uid           = u->uid;
    pd.flags         = u->flags;

    pdfs_set_context(NULL, 1);    /* elevated: /usr is root-only */

    cr = vfs_create(path);
    if (cr != 0 && cr != -3) {    /* -3 = already exists, that is fine */
        pdfs_set_context(NULL, 0);
        u_memset(&pd, 0, (int)sizeof(pduc_t));
        return -2;
    }
    if (cr == 0)
        pdfs_chmod(path, PDFS_MODE_RUSR | PDFS_MODE_WUSR); /* rw------- */

    if (vfs_open(path, &node) != 0) {
        pdfs_set_context(NULL, 0);
        u_memset(&pd, 0, (int)sizeof(pduc_t));
        return -3;
    }

    vfs_write(&node, 0u, (uint32_t)sizeof(pduc_t), &pd);

    pdfs_set_context(NULL, 0);
    u_memset(&pd, 0, (int)sizeof(pduc_t));  /* zero sensitive data */
    return 0;
}

void users_load_from_disk(void)
{
    uint32_t      idx;
    pdfs_dirent_t de;

    pdfs_set_context(NULL, 1);    /* elevated: /usr is root-only */

    for (idx = 0u; ; idx++) {
        if (pdfs_stat_dir("usr", idx, &de) != 0) break;
        if (de.flags & PDFS_FLAG_DIR)          continue;
        if (!has_pduc_suffix(de.name))         continue;

        /* Build "/usr/<name>" */
        char path[40];
        int  j = 0;
        int  k;
        path[j++]='/'; path[j++]='u'; path[j++]='s';
        path[j++]='r'; path[j++]='/';
        for (k = 0; de.name[k] && j < 38; k++) path[j++] = de.name[k];
        path[j] = '\0';

        vfs_node_t node;
        if (vfs_open(path, &node)  != 0)                 continue;
        if (node.size < (uint32_t)sizeof(pduc_t))         continue;

        pduc_t pd;
        u_memset(&pd, 0, (int)sizeof(pduc_t));
        if (vfs_read(&node, 0u, (uint32_t)sizeof(pduc_t), &pd)
                != (int)sizeof(pduc_t)) {
            u_memset(&pd, 0, (int)sizeof(pduc_t));
            continue;
        }

        /* Validate: null-terminate just in case */
        pd.username[USERNAME_LEN - 1] = '\0';
        if (!pd.username[0]) { u_memset(&pd, 0, (int)sizeof(pduc_t)); continue; }

        /*
         * UID 0 (root) and 1 (pd) are hardcoded; update their hash only.
         * UID 2+ are new dynamic users; add them to the table.
         */
        if (pd.uid == 0u)
            update_hash_by_name("root", pd.password_hash);
        else if (pd.uid == 1u)
            update_hash_by_name("pd",   pd.password_hash);
        else
            add_raw(pd.username, pd.password_hash, pd.uid, pd.flags);

        u_memset(&pd, 0, (int)sizeof(pduc_t));  /* zero sensitive data */
    }

    pdfs_set_context(NULL, 0);
}
