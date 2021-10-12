#include "main/php.h"

struct _byteBuffer {
    char *elems;
    int length;
}; 

typedef struct _byteBuffer byteBuffer; 

/* Prototypes */
byteBuffer *newByteBuffer(int size);
void resetBuffer(byteBuffer *buffer);
void appendByte(byteBuffer *buffer, int v);
void appendBytes(byteBuffer *buffer, char *bs, int bs_len);
void appendChar(byteBuffer *buffer, int v);
void appendInt(byteBuffer *buffer, int v);
void appendLong(byteBuffer *buffer, long v);
void appendDouble(byteBuffer *buffer, double v);
void appendFloat(byteBuffer *buffer, float v);
void appendIntByPos(byteBuffer *buffer, int x, int pos);
void freeBuffer(byteBuffer *buffer);


struct _constPool {
    HashTable *vars;
    int last_var;
};
typedef struct _constPool constPool;

struct _nameAndType {
    char *name;
    char *type;
};
typedef struct _nameAndType nameAndType;

struct _refInfo {
    char *class;
    int type;
    nameAndType *nameType;
};
typedef struct _refInfo refInfo;

constPool *newConstPool(int size);
int putValue(constPool *p, zval *val);
void freeConstPool(constPool *p);

int getPoolSize(constPool *p);

int putClassValue(constPool *p, zend_string *str);
int putStringValue(constPool *p, zend_string *str);

int getClassValue(constPool *p, zend_string *str);
int getStringValue(constPool *p, zend_string *str);

int getUtfValue(constPool *p, zend_string *str);
int putUtfValue(constPool *p, zend_string *str);

int getLongValue(constPool *p, zend_long l);
int putLongValue(constPool *p, zend_long l);
