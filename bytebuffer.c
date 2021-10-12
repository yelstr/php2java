#include "bytebuffer.h"
#include <string.h>

static int pool_size = 16;

//todo 容量检测，不够的需要扩容
byteBuffer *newByteBuffer(int size)
{
    byteBuffer *buffer;
    char *elems = emalloc(sizeof(char) * size);
    if (elems == NULL) {
        zend_error(E_ERROR, "bytebuffer size malloc memory error!");
        return NULL;
    }

    buffer = emalloc(sizeof(byteBuffer));
    if (buffer == NULL) {
        zend_error(E_ERROR, "bytebuffer size malloc memory error!");
        return NULL;
    }

    buffer->elems = elems;
    buffer->length = 0;
    memset(buffer->elems, '\0', size);
    return buffer;
}

void resetBuffer(byteBuffer *buffer) {
    buffer->length = 0;
}

void freeBuffer(byteBuffer *buffer)
{
    if (buffer != NULL) {
        if (buffer->elems != NULL) {
            efree(buffer->elems);
        }
        efree(buffer); 
    }
}

/** Append byte to this buffer.
 */
void appendByte(byteBuffer *buffer, int x)
{
    buffer->elems[buffer->length] = x & 0xFF;
    buffer->length++;
}

/** Append `len' bytes from byte array,
 *  starting at given `start' offset.
 */
void appendBytes(byteBuffer *buffer, char *bs, int bs_len)
{
    memcpy(buffer->elems + buffer->length, bs, bs_len);
    buffer->length = buffer->length + bs_len;
}

/** Append a character as a two byte number.
 */
void appendChar(byteBuffer *buffer, int x)
{
    buffer->elems[buffer->length] = (x >> 8) & 0xFF;
    buffer->elems[buffer->length+1] = x & 0xFF;
    buffer->length = buffer->length + 2;
}

void appendCharByPos(byteBuffer *buffer, int x, int pos)
{
    buffer->elems[pos] = (x >> 8) & 0xFF;
    buffer->elems[pos+1] = x & 0xFF;
}

void appendByteByPos(byteBuffer *buffer, int x, int pos)
{
    buffer->elems[pos] = (x >> 8) & 0xFF;
}

void appendIntByPos(byteBuffer *buffer, int pos, int x)
{
    buffer->elems[pos  ] = (x >> 24) & 0xFF;
    buffer->elems[pos+1] = (x >> 16) & 0xFF;
    buffer->elems[pos+2] = (x >> 8) & 0xFF;
    buffer->elems[pos+3] = x & 0xFF;
}

/** Append an integer as a four byte number.
 */
void appendInt(byteBuffer *buffer, int x)
{
    buffer->elems[buffer->length  ] = (x >> 24) & 0xFF;
    buffer->elems[buffer->length+1] = (x >> 16) & 0xFF;
    buffer->elems[buffer->length+2] = (x >> 8) & 0xFF;
    buffer->elems[buffer->length+3] = x & 0xFF;
    buffer->length = buffer->length + 4;
}

/** Append a long as an eight byte number.
 */
void appendLong(byteBuffer *buffer, long x)
{
    buffer->elems[buffer->length  ] = (x >> 56) & 0xFF;
    buffer->elems[buffer->length+1] = (x >> 48) & 0xFF;
    buffer->elems[buffer->length+2] = (x >> 40) & 0xFF;
    buffer->elems[buffer->length+3] = (x >> 32) & 0xFF;
    buffer->elems[buffer->length+4] = (x >> 24) & 0xFF;
    buffer->elems[buffer->length+5] = (x >> 16) & 0xFF;
    buffer->elems[buffer->length+6] = (x >> 8) & 0xFF;
    buffer->elems[buffer->length+7] = x & 0xFF;
    buffer->length = buffer->length + 8;
}

void appendDouble(byteBuffer *buffer, double v)
{
    buffer->length = buffer->length + 8;
}

void appendFloat(byteBuffer *buffer, float v)
{
    buffer->length = buffer->length + 4;
}

static zend_string *zend_string_cat_str(zend_string *str, const char *s)
{
    zend_string *new_str = zend_string_alloc(ZSTR_LEN(str) + strlen(s), 0);
    memcpy(ZSTR_VAL(new_str), s, strlen(s));    
    memcpy(ZSTR_VAL(new_str) + strlen(s), ZSTR_VAL(str), ZSTR_LEN(str));    
    return new_str;
}

static zend_bool zval_is_equal(zval *v1, zval *v2)
{
    if (v1 == NULL || v2 == NULL) {
        return 0;
    }

    if (Z_TYPE_P(v1) == Z_TYPE_P(v2) && Z_TYPE_P(v2) == IS_STRING) {

        if (Z_STRVAL_P(v1) == Z_STRVAL_P(v2) ||
            (Z_STRHASH_P(v1) == Z_STRHASH_P(v2) &&
		    Z_STRLEN_P(v1) == Z_STRLEN_P(v2) &&
		    memcmp(Z_STRVAL_P(v1), Z_STRVAL_P(v2), Z_STRLEN_P(v1)) == 0)) {
            return 1;
        }

    } else if (Z_TYPE_P(v1) == Z_TYPE_P(v2) && Z_TYPE_P(v2) == IS_LONG) {
        if (Z_LVAL_P(v1) == Z_LVAL_P(v2)) {
            return 1;
        }
    } else if (Z_TYPE_P(v1) == Z_TYPE_P(v2) && Z_TYPE_P(v2) == IS_DOUBLE) {
        if (Z_DVAL_P(v1) == Z_DVAL_P(v2)) {
            return 1;
        }
    }
    return 0;
}

// 常量池相关操作
constPool *newConstPool(int size)
{
    constPool *p = emalloc(sizeof(constPool));
    if (p == NULL) {
        zend_error(E_ERROR, "init java constpool error, malloc memory error!");
        return NULL;
    }

    HashTable *ht = emalloc(sizeof(HashTable));
    zend_hash_init(ht, size, NULL, ZVAL_PTR_DTOR, 0);
    
    p->vars = ht;
    p->last_var = 1;
    return p;
}

int putClassValue(constPool *p, zend_string *str)
{
    zend_string *new_str = zend_string_cat_str(str, "class#");

    int idx = getUtfValue(p, new_str);
    if (idx != -1) {
        return idx;
    }

    zval val;
    ZVAL_LONG(&val, p->last_var);

    zend_hash_index_add(p->vars, ZSTR_H(new_str), &val);

    zend_string_free(new_str);
    return p->last_var ++;
}

int getUtfValue(constPool *p, zend_string *str)
{
    if (!ZSTR_H(str)) {
		ZSTR_H(str) = zend_hash_func(ZSTR_VAL(str), ZSTR_LEN(str));
	}

    zval *find = _zend_hash_index_find(p->vars, ZSTR_H(str));    

    if (find != NULL) {
        return Z_LVAL_P(find);
    }
    return -1;
}

int putUtfValue(constPool *p, zend_string *str)
{
    int idx = getUtfValue(p, str);
    if (idx != -1) {
        return idx;
    }

    zval val;
    ZVAL_LONG(&val, p->last_var);

    zend_hash_index_add(p->vars, ZSTR_H(str), &val);

    return p->last_var ++;
}

int putValue(constPool *p, zval *val)
{
    zend_ulong h;

    switch (Z_TYPE_P(val))
    {
    case IS_STRING:
        h = Z_STRHASH_P(val);
        break;
    case IS_LONG:
        h = Z_LVAL_P(val);
        break; 
    case IS_DOUBLE:
        h = (zend_ulong)Z_DVAL_P(val);
        break;
    default:
        break;
    }

    zval *find = _zend_hash_index_find(p->vars, h); 
    if (find != NULL) {
        return Z_LVAL_P(find);
    }

    zval add_val;
    ZVAL_LONG(&add_val, p->last_var);

    zend_hash_index_add(p->vars, h, &add_val);

    return p->last_var ++;
}

int getPoolSize(constPool *p)
{
    return p->last_var;
}

int getClassValue(constPool *p, zend_string *str)
{
    zend_string *new_str = zend_string_cat_str(str, "class#");
    int idx = getUtfValue(p, new_str);
    zend_string_free(new_str);
    return idx;
}

int putStringValue(constPool *p, zend_string *str)
{
    zend_string *new_str = zend_string_cat_str(str, "string#");

    int idx = getUtfValue(p, new_str);
    if (idx != -1) {
        return idx;
    }

    zval val;
    ZVAL_LONG(&val, p->last_var);

    zend_hash_index_add(p->vars, ZSTR_H(new_str), &val);

    zend_string_free(new_str);

    return p->last_var ++;
}

int getStringValue(constPool *p, zend_string *str)
{
    zend_string *new_str = zend_string_cat_str(str, "string#");
    int idx = getUtfValue(p, str);
    zend_string_free(new_str);
    return idx;
}

int getLongValue(constPool *p, zend_long l)
{
    zval *find = _zend_hash_index_find(p->vars, l);
    return find == NULL ? -1 : Z_LVAL_P(find);
}

int putLongValue(constPool *p, zend_long l)
{
    int idx = getLongValue(p, l);
    if (idx != -1) {
        return idx; 
    }
    zval val;
    ZVAL_LONG(&val, p->last_var);

    zend_hash_index_add(p->vars, l, &val);

    return p->last_var ++;
}

void freeConstPool(constPool *p)
{
    if (p) {
        if (p->vars) {
            zend_hash_destroy(p->vars);
            efree(p->vars); 
        }
        efree(p);
    }
}