#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    char header[64];
    const char *type_str = (type == OBJ_BLOB) ? "blob" : (type == OBJ_TREE) ? "tree" : "commit";
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len);
    header_len++; 

    uint8_t *full_buf = malloc(header_len + len);
    if (!full_buf) return -1;
    memcpy(full_buf, header, header_len);
    memcpy(full_buf + header_len, data, len);
    compute_hash(full_buf, header_len + len, id_out);

    if (object_exists(id_out)) {
        free(full_buf);
        return 0; 
    }

    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id_out, hex);
    char dir_path[512];
    snprintf(dir_path, sizeof(dir_path), "%s/%.2s", OBJECTS_DIR, hex);
    mkdir(dir_path, 0755); 

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s/tmp_XXXXXX", dir_path);
    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        free(full_buf);
        return -1;
    }
    
    if (write(fd, full_buf, header_len + len) != (ssize_t)(header_len + len)) {
        close(fd); unlink(tmp_path); free(full_buf); return -1;
    }

    fsync(fd);
    close(fd);
    free(full_buf);

    char final_path[512];
    object_path(id_out, final_path, sizeof(final_path));
    if (rename(tmp_path, final_path) != 0) {
        unlink(tmp_path); return -1;
    }
    return 0;
}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < 0) {
        fclose(f);
        return -1;
    }

    uint8_t *full_buf = malloc(file_size);
    if (!full_buf) {
        fclose(f);
        return -1;
    }

    if (fread(full_buf, 1, file_size, f) != (size_t)file_size) {
        free(full_buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    ObjectID computed_id;
    compute_hash(full_buf, file_size, &computed_id);
    if (memcmp(id->hash, computed_id.hash, HASH_SIZE) != 0) {
        free(full_buf);
        return -1; 
    }

    uint8_t *null_byte = memchr(full_buf, '\0', file_size);
    if (!null_byte) {
        free(full_buf);
        return -1; 
    }

    char type_str[16];
    size_t claimed_size;
    if (sscanf((char *)full_buf, "%15s %zu", type_str, &claimed_size) != 2) {
        free(full_buf);
        return -1;
    }

    if (strcmp(type_str, "blob") == 0) {
        *type_out = OBJ_BLOB;
    } else if (strcmp(type_str, "tree") == 0) {
        *type_out = OBJ_TREE;
    } else if (strcmp(type_str, "commit") == 0) {
        *type_out = OBJ_COMMIT;
    } else {
        free(full_buf);
        return -1;
    }

    size_t header_len = (null_byte - full_buf) + 1;
    if (claimed_size != (size_t)(file_size - header_len)) {
        free(full_buf);
        return -1; 
    }

    *len_out = claimed_size;
    *data_out = malloc(claimed_size);
    if (!*data_out) {
        free(full_buf);
        return -1;
    }

    memcpy(*data_out, full_buf + header_len, claimed_size);
    free(full_buf);

    return 0;
}