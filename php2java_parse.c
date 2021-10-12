#include <stdio.h>
#include <string.h>

#include "main/php.h"

#include "java_classfile_constants.h"
#include "php2java_parse.h"

#include "Zend/zend.h"
#include "Zend/zend_compile.h"
#include "Zend/zend_interfaces.h"
#include "Zend/zend_exceptions.h"
#include "Zend/zend_language_scanner.h"
#include "bytebuffer.h"

#define MAXLEN 1024

#define FC(member) (CG(file_context).member)

unsigned char opcode_length[JVM_OPC_MAX + 1] = JVM_OPCODE_LENGTH_INITIALIZER;

static const int data_buf_size = 0x0fff0;
static const int pool_buf_size = 0x1fff0;

static byteBuffer *poolbuf = NULL;
static byteBuffer *databuf = NULL;
static byteBuffer *linebuf = NULL;
static constPool *pool = NULL;
static int code_start_pc = 0;

static void php2java_startup()
{
    zend_interned_strings_activate();
    zend_activate();
	poolbuf = newByteBuffer(pool_buf_size);
	databuf = newByteBuffer(data_buf_size);
	linebuf = newByteBuffer(128);
	pool = newConstPool(0xff);
}

/**
 * 函数转化操作
 * echo/var_dump -> System.out.print
 * printf -> System.out.printf
 **/

static void php2java_shutdown()
{
    zend_interned_strings_deactivate();
    zend_deactivate();
	freeBuffer(poolbuf);
	freeBuffer(databuf);
	freeBuffer(linebuf);
	freeConstPool(pool);
}

static void init_java_klass_file_headear(char *s, int java_version) {
    s[0] = 0xCA;
    s[1] = 0xFE;
    s[2] = 0xBA;
    s[3] = 0xBE;
    s[4] = 0x00;
    s[5] = 0x00;
    s[6] = 0x00;
    switch (java_version)
    {
    case JAVA_SE_9:
        s[7] = JAVA_9_V;
        break;
    case JAVA_SE_10:
        s[7] = JAVA_10_V;
        break;
    case JAVA_SE_11:
        s[7] = JAVA_11_V;
        break;
    default: 
        s[7] = JAVA_8_V;
        break;
    }
}

static char *to_java_filename(char *filename) {
	//todo
	char realpath[256] = {'\0'};
	VCWD_REALPATH(filename, realpath);
	char *ret = NULL;
	char *class_str = ".class";

	//获取当前文件名
	// zend_string *java_name = php_basename(filename, strlen(filename), ".php", 4);
	// zend_string *java_name = php_basename("test/0001.php", strlen("test/0001.php"), ".php", 4);


	size_t len = zend_dirname(realpath, strlen(realpath));
	ret = emalloc(sizeof(char) * strlen("tests/001"));

	// memcpy(ret, realpath, len);
	// strcat(ret, "001");
	// strcat(ret, class_str);

	return ret;
}

static int appendUtf8(char *s)
{
	zend_string *str = zend_string_init(s, strlen(s), 0);
	int idx = getUtfValue(pool, str);
	if (idx == -1) {
		appendByte(poolbuf, JVM_CONSTANT_Utf8);
		appendChar(poolbuf, strlen(s));
		appendBytes(poolbuf, s, strlen(s));
		idx = putUtfValue(pool, str);
	}
	zend_string_free(str);
	return idx;
}

static int appendIntegerValue(zend_long l)
{
	int idx = getLongValue(pool, l);
	if (idx == -1) {
		idx = putLongValue(pool, l);
		appendByte(poolbuf, JVM_CONSTANT_Integer);
		appendInt(poolbuf, l);
	}
	return idx;	
}

static int appendLongValue(zend_long l)
{
	int idx = getLongValue(pool, l);
	if (idx == -1) {
		idx = putLongValue(pool, l);
		appendByte(poolbuf, JVM_CONSTANT_Long);
		appendLong(poolbuf, l);
	}
	return idx;	
}

static int appendString(zend_string *str)
{
	int idx = getStringValue(pool, str);
	if (idx == -1) {
		int s_idx = appendUtf8(ZSTR_VAL(str));
		appendByte(poolbuf, JVM_CONSTANT_String);
		appendChar(poolbuf, s_idx);
		idx = putStringValue(pool, str);
	}
	return idx;	
}

static int appendNameAndTypeValue(nameAndType *nameType)
{
    char tmp[1024];
	memset(tmp, '\0', 1024);
    sprintf(tmp, "%s:%s", nameType->name, nameType->type);

	zend_string *s = zend_string_init(tmp, strlen(nameType->name)+strlen(nameType->type)+1, 0);

    int idx = getUtfValue(pool, s); 
    if (idx == -1) {
        int name_idx = appendUtf8(nameType->name);
        int type_idx = appendUtf8(nameType->type);
        idx = putUtfValue(pool, s);
		appendByte(poolbuf, JVM_CONSTANT_NameAndType);
		appendChar(poolbuf, name_idx);
		appendChar(poolbuf, type_idx);
    }

	zend_string_free(s);
    return idx;
}

static int appendClass(char *s)
{
	zend_string *str = zend_string_init(s, strlen(s), 0);
	int idx = getClassValue(pool, str);
	if (idx == -1) {
		int s_idx = appendUtf8(s);
		appendByte(poolbuf, JVM_CONSTANT_Class);
		appendChar(poolbuf, s_idx);
		idx = putClassValue(pool, str);
	}
	zend_string_free(str);
	return idx;
}

static int appendRefValue(refInfo *ref_info) {
	char tmp[1024];
	memset(tmp, '\0', 1024);
	sprintf(tmp, "%s.%s:%s", ref_info->class, ref_info->nameType->name, ref_info->nameType->type);
	size_t len = strlen(ref_info->class)+strlen(ref_info->nameType->name)+strlen(ref_info->nameType->type) + 2;
	zend_string *s = zend_string_init(tmp, len, 0);	
	int idx = getUtfValue(pool, s); 
    if (idx == -1) {
		int class_idx = appendClass(ref_info->class);
		int name_type_idx = appendNameAndTypeValue(ref_info->nameType);
		appendByte(poolbuf, ref_info->type);
		appendChar(poolbuf, class_idx);
		appendChar(poolbuf, name_type_idx);
        idx = putUtfValue(pool, s);
    }

	zend_string_free(s);
    return idx;
}

static void init_java_klass_constant()
{
	appendClass(java_ParentObjectStr);
	appendUtf8(java_Code);
	appendUtf8(java_ConstantValue);
	appendUtf8(java_LineNumberTable);
	appendUtf8(java_LocalVariableTypeTable);
	appendUtf8(java_MainRetSignture);
	appendUtf8(java_Signature);
	appendUtf8(java_SourceFile);
	appendUtf8(java_StackMap);
	appendUtf8(java_StackMapTable);
	appendUtf8(java_MainFunc);
}

static int begin_attrs()
{
	appendChar(databuf, 0);	
	return databuf->length;
}

static void end_attr(int idx)
{
	appendIntByPos(databuf, idx - 4, databuf->length - idx);
}

static void append_java_init_method()
{
	resetBuffer(linebuf);
	appendChar(databuf, JVM_ACC_PUBLIC);
	appendChar(databuf, appendUtf8(java_ObjectInitFunc));
	appendChar(databuf, appendUtf8(java_VoidRet));
	appendChar(databuf, 1);

	appendChar(databuf, appendUtf8(java_Code));
	appendInt(databuf, 0);
	int attr_len = databuf->length;
	
	appendChar(databuf, 1);
	appendChar(databuf, 1);

	appendInt(databuf, 0);
	int code_len = databuf->length;

	// 构造方法定义
	refInfo init_method_ref;
	nameAndType void_type;

	void_type.name = java_ObjectInitFunc;
	void_type.type = java_VoidRet;
	init_method_ref.type = JVM_CONSTANT_Methodref;
	init_method_ref.nameType = &void_type;
	init_method_ref.class = java_ParentObjectStr;

	appendByte(databuf, JVM_OPC_aload_0);
	appendByte(databuf, JVM_OPC_invokespecial);
	appendChar(databuf, appendRefValue(&init_method_ref));

	//函数结束之后添加return指令
	appendByte(databuf, JVM_OPC_return);
	end_attr(code_len);

	appendChar(linebuf, 0);
	appendChar(linebuf, 1);

	// 异常表 exception_table_length
	appendChar(databuf, 0);

	//lineNumberTable 每条指令对应的源文件行号
	appendChar(databuf, 1);
	appendChar(databuf, appendUtf8(java_LineNumberTable));
	appendInt(databuf, linebuf->length + 2);
	if (linebuf->length > 0) {
		appendChar(databuf, linebuf->length / 4);
		appendBytes(databuf, linebuf->elems, linebuf->length);
	} else {
		appendChar(databuf, 0);
	}

	end_attr(attr_len);

	resetBuffer(linebuf);
}

//todo 待完善
static void php2java_assign(zend_op_array *op_array, zend_op *op)
{
	uint32_t var_num;
	int idx;

	if (op->op2_type == IS_CONST && (op->op1_type & (IS_CV|IS_VAR|IS_TMP_VAR))) {

		var_num = EX_VAR_TO_NUM(op->op1.var);

		zval *constant = (zval *)(((char *)op_array->literals) + op->op2.constant);

		if (Z_TYPE_P(constant) == IS_STRING) {

			appendChar(linebuf, code_start_pc);
			appendChar(linebuf, op->lineno);
			code_start_pc ++;

			idx = appendString(Z_STR_P(constant));
			appendByte(databuf, JVM_OPC_ldc);
			appendByte(databuf, idx);

			if (var_num == 0) {
				appendByte(databuf, JVM_OPC_astore_1);
			} else if (var_num == 1) {
				appendByte(databuf, JVM_OPC_astore_2);
			} else if (var_num == 2) {
				appendByte(databuf, JVM_OPC_astore_3);
			} else {
				appendByte(databuf, JVM_OPC_astore);
				appendChar(databuf, var_num);
			}

		} else if (Z_TYPE_P(constant) == IS_LONG) {

			appendChar(linebuf, code_start_pc);
			appendChar(linebuf, op->lineno);
			code_start_pc ++;

			if (Z_LVAL_P(constant) < -0xffffffff) {
				idx = appendLongValue(Z_LVAL_P(constant));
				appendByte(databuf, JVM_OPC_ldc2_w);
				appendChar(databuf, idx);
			} else if (Z_LVAL_P(constant) < -0x7fff) {	
				idx = appendIntegerValue(Z_LVAL_P(constant));
				appendByte(databuf, JVM_OPC_ldc);
				appendByte(databuf, idx);
			} else if (-0x7fff <= Z_LVAL_P(constant) && Z_LVAL_P(constant) < -1) {
				appendByte(databuf, JVM_OPC_sipush);
				appendChar(databuf, Z_LVAL_P(constant));
			} else if (Z_LVAL_P(constant) == -1) {
				appendByte(databuf, JVM_OPC_iconst_m1);	
			} else if (Z_LVAL_P(constant) == 1) {
				appendByte(databuf, JVM_OPC_iconst_1);
			} else if (Z_LVAL_P(constant) == 2) {
				appendByte(databuf, JVM_OPC_iconst_2);	
			} else if (Z_LVAL_P(constant) == 3) {
				appendByte(databuf, JVM_OPC_iconst_3);	
			} else if (Z_LVAL_P(constant) < 0x7fff) {
				appendByte(databuf, JVM_OPC_sipush);	
				appendChar(databuf, Z_LVAL_P(constant));
			} else if (Z_LVAL_P(constant) < 0xffffffff) {
				idx = appendIntegerValue(Z_LVAL_P(constant));
				appendByte(databuf, JVM_OPC_ldc);	
				appendByte(databuf, idx);
			} else {
				idx = appendLongValue(Z_LVAL_P(constant));
				appendByte(databuf, JVM_OPC_ldc2_w);	
				appendChar(databuf, idx);
			}

			if (var_num == 0) {
				appendByte(databuf, JVM_OPC_istore_1);
			} else if (var_num == 1) {
				appendByte(databuf, JVM_OPC_istore_2);
			} else if (var_num == 2) {
				appendByte(databuf, JVM_OPC_istore_3);
			} else {
				appendByte(databuf, JVM_OPC_istore);
				appendChar(databuf, var_num);
			}

		} else if (Z_TYPE_P(constant) == IS_DOUBLE) {

		}
	}
}

static void php2java_echo(zend_op_array *op_array, zend_op *op)
{
	char *print_str = "print";
	char *print_stream_class = "java/io/PrintStream";
	char *system_class = "java/lang/System";
	char *out_name = "out";
	char *out_type = "Ljava/io/PrintStream;";

	char print_ret_type[100];
	memset(print_ret_type, '\0', 100);

	uint32_t var_num;
	int idx;
	nameAndType out_name_type;
	nameAndType print_name_type;

	out_name_type.name = out_name;
	out_name_type.type = out_type;

	refInfo out_field_ref;
	out_field_ref.class = system_class;
	out_field_ref.nameType = &out_name_type;
	out_field_ref.type = JVM_CONSTANT_Fieldref;

	int out_field_idx = appendRefValue(&out_field_ref);
	appendByte(databuf, JVM_OPC_getstatic);
	appendChar(databuf, out_field_idx);

	if (op->op1_type == IS_CONST) {
		zval *constant = (zval *)(((char *)op_array->literals) + op->op1.constant);

		if (Z_TYPE_P(constant) == IS_STRING) {

			appendChar(linebuf, code_start_pc);
			appendChar(linebuf, op->lineno);
			code_start_pc ++;

			idx = appendString(Z_STR_P(constant));
			appendByte(databuf, JVM_OPC_ldc);
			appendByte(databuf, idx);
			sprintf(print_ret_type, "(%s)V", "Ljava/lang/String;");

		} else if (Z_TYPE_P(constant) == IS_LONG) {

			appendChar(linebuf, code_start_pc);
			appendChar(linebuf, op->lineno);
			code_start_pc ++;

			if (Z_LVAL_P(constant) < -0xffffffff) {
				idx = appendLongValue(Z_LVAL_P(constant));
				appendByte(databuf, JVM_OPC_ldc2_w);
				appendChar(databuf, idx);
				sprintf(print_ret_type, "(%c)V", JVM_SIGNATURE_LONG);
			} else if (Z_LVAL_P(constant) < -0x7fff) {	
				idx = appendIntegerValue(Z_LVAL_P(constant));
				appendByte(databuf, JVM_OPC_ldc);
				appendByte(databuf, idx);
				sprintf(print_ret_type, "(%c)V", JVM_SIGNATURE_INT);
			} else if (-0x7fff <= Z_LVAL_P(constant) && Z_LVAL_P(constant) < -1) {
				appendByte(databuf, JVM_OPC_sipush);
				appendChar(databuf, Z_LVAL_P(constant));
				sprintf(print_ret_type, "(%c)V", JVM_SIGNATURE_INT);
			} else if (Z_LVAL_P(constant) == -1) {
				appendByte(databuf, JVM_OPC_iconst_m1);	
				sprintf(print_ret_type, "(%c)V", JVM_SIGNATURE_INT);
			} else if (Z_LVAL_P(constant) == 1) {
				appendByte(databuf, JVM_OPC_iconst_1);
				sprintf(print_ret_type, "(%c)V", JVM_SIGNATURE_INT);
			} else if (Z_LVAL_P(constant) == 2) {
				appendByte(databuf, JVM_OPC_iconst_2);	
				sprintf(print_ret_type, "(%c)V", JVM_SIGNATURE_INT);
			} else if (Z_LVAL_P(constant) == 3) {
				appendByte(databuf, JVM_OPC_iconst_3);	
				sprintf(print_ret_type, "(%c)V", JVM_SIGNATURE_INT);
			} else if (Z_LVAL_P(constant) < 0x7fff) {
				appendByte(databuf, JVM_OPC_sipush);	
				appendChar(databuf, Z_LVAL_P(constant));
				sprintf(print_ret_type, "(%c)V", JVM_SIGNATURE_INT);
			} else if (Z_LVAL_P(constant) < 0xffffffff) {
				idx = appendIntegerValue(Z_LVAL_P(constant));
				appendByte(databuf, JVM_OPC_ldc);
				appendByte(databuf, idx);
				sprintf(print_ret_type, "(%c)V", JVM_SIGNATURE_INT);
			} else {
				idx = appendLongValue(Z_LVAL_P(constant));
				appendByte(databuf, JVM_OPC_ldc2_w);	
				appendChar(databuf, idx);
				sprintf(print_ret_type, "(%c)V", JVM_SIGNATURE_LONG);
			}

		} else if (Z_TYPE_P(constant) == IS_DOUBLE) {

		}

	} else if (op->op1_type & (IS_CV|IS_VAR)) {
		var_num = EX_VAR_TO_NUM(op->op1.var);
	}

	print_name_type.name = print_str;
	print_name_type.type = print_ret_type;


	//输出方法写入
	refInfo print_method_ref;
	print_method_ref.class = print_stream_class;
	print_method_ref.nameType = &print_name_type;
	print_method_ref.type = JVM_CONSTANT_Methodref;

	appendByte(databuf, JVM_OPC_invokevirtual);	
	appendChar(databuf, appendRefValue(&print_method_ref));
}

static void phpj2ava_new(zend_op_array *op_array, zend_op *op)
{

}

static void process_op_array(zend_op_array *op_array)
{
	appendChar(databuf, 2); //max_stacks
	appendChar(databuf, op_array->last_var + 1); //max_locals，this变量占一个参数位

	appendInt(databuf, 0); //code_length
	int alen_idx = databuf->length;

	zend_op *op = op_array->opcodes;
    zend_op *end = op_array->opcodes + op_array->last;

    while (op < end)
    {
        switch (op->opcode)
        {
        case ZEND_NOP:
            break;
        case ZEND_ADD:
            break;
        case ZEND_SUB:
            break;
        case ZEND_MUL:
            break;
        case ZEND_DIV:
            break;
        case ZEND_MOD:
            break;
        case ZEND_SL:
            break;
        case ZEND_SR:
            break;
        case ZEND_CONCAT:
            break;
        case ZEND_BW_OR:
            break;
        case ZEND_BW_AND:
            break;
        case ZEND_BW_XOR:
            break;
        case ZEND_BW_NOT:
            break;
        case ZEND_BOOL_NOT:
            break;
        case ZEND_BOOL_XOR:
            break;
        case ZEND_IS_IDENTICAL:
            break;
        case ZEND_IS_NOT_IDENTICAL:
            break;
        case ZEND_IS_EQUAL:
            break;
        case ZEND_IS_NOT_EQUAL:
            break;
        case ZEND_IS_SMALLER:
            break;
        case ZEND_IS_SMALLER_OR_EQUAL:
            break;
        case ZEND_CAST:
            break;
        case ZEND_QM_ASSIGN:
            break;
        case ZEND_ASSIGN_ADD:
            break;
        case ZEND_ASSIGN_SUB:
            break;
        case ZEND_ASSIGN_MUL:
            break;
        case ZEND_ASSIGN_DIV:
            break;
        case ZEND_ASSIGN_MOD:
            break;
        case ZEND_ASSIGN_SL:
            break;
        case ZEND_ASSIGN_SR:
            break;
        case ZEND_ASSIGN_CONCAT:
            break;
        case ZEND_ASSIGN_BW_OR:
            break;
        case ZEND_ASSIGN_BW_AND:
            break;
        case ZEND_ASSIGN_BW_XOR:
            break;
        case ZEND_PRE_INC:
            break;
        case ZEND_PRE_DEC:
            break;
        case ZEND_POST_INC:
            break;
        case ZEND_POST_DEC:
            break;
        case ZEND_ASSIGN:
			php2java_assign(op_array, op);
            break;
        case ZEND_ASSIGN_REF:
            break;
        case ZEND_ECHO:
			php2java_echo(op_array, op);
            break;
        case ZEND_GENERATOR_CREATE:
            break;
        case ZEND_JMP:
            break;
        case ZEND_JMPZ:
            break;
        case ZEND_JMPNZ:
            break;
        case ZEND_JMPZNZ:
            break;
        case ZEND_JMPZ_EX:
            break;
        case ZEND_JMPNZ_EX:
            break;
        case ZEND_CASE:
            break;
        case ZEND_CHECK_VAR:
            break;
        case ZEND_SEND_VAR_NO_REF_EX:
            break;
        case ZEND_MAKE_REF:
            break;
        case ZEND_BOOL:
            break;
        case ZEND_FAST_CONCAT:
            break;
        case ZEND_ROPE_INIT:
            break;
        case ZEND_ROPE_ADD:
            break;
        case ZEND_ROPE_END:
            break;
        case ZEND_BEGIN_SILENCE:
            break;
        case ZEND_END_SILENCE:
            break;
        case ZEND_INIT_FCALL_BY_NAME:
            break;
        case ZEND_DO_FCALL:
            break;
        case ZEND_INIT_FCALL:
            break;
        case ZEND_RETURN:
            break;
        case ZEND_RECV:
            break;
        case ZEND_RECV_INIT:
            break;
        case ZEND_SEND_VAL:
            break;
        case ZEND_SEND_VAR_EX:
            break;
        case ZEND_SEND_REF:
            break;
        case ZEND_NEW:
			php2java_new();
            break;
        case ZEND_INIT_NS_FCALL_BY_NAME:
            break;
        case ZEND_FREE:
            break;
        case ZEND_INIT_ARRAY:
            break;
        case ZEND_ADD_ARRAY_ELEMENT:
            break;
        case ZEND_INCLUDE_OR_EVAL:
            break;
        case ZEND_UNSET_VAR:
            break;
        case ZEND_UNSET_DIM:
            break;
        case ZEND_UNSET_OBJ:
            break;
        case ZEND_FE_RESET_R:
            break;
        case ZEND_FE_FETCH_R:
            break;
        case ZEND_EXIT:
            break;
        case ZEND_FETCH_R:
            break;
        case ZEND_FETCH_DIM_R:
            break;
        case ZEND_FETCH_OBJ_R:
            break;
        case ZEND_FETCH_W:
            break;
        case ZEND_FETCH_DIM_W:
            break;
        case ZEND_FETCH_OBJ_W:
            break;
        case ZEND_FETCH_RW:
            break;
        case ZEND_FETCH_DIM_RW:
            break;
        case ZEND_FETCH_OBJ_RW:
            break;
        case ZEND_FETCH_IS:
            break;
        case ZEND_FETCH_DIM_IS:
            break;
        case ZEND_FETCH_OBJ_IS:
            break;
        case ZEND_FETCH_FUNC_ARG:
            break;
        case ZEND_FETCH_DIM_FUNC_ARG:
            break;
        case ZEND_FETCH_OBJ_FUNC_ARG:
            break;
        case ZEND_FETCH_UNSET:
            break;
        case ZEND_FETCH_DIM_UNSET:
            break;
        case ZEND_FETCH_OBJ_UNSET:
            break;
        case ZEND_FETCH_LIST:
            break;
        case ZEND_FETCH_CONSTANT:
            break;
        case ZEND_EXT_STMT:
            break;
        case ZEND_EXT_FCALL_BEGIN:
            break;
        case ZEND_EXT_FCALL_END:
            break;
        case ZEND_EXT_NOP:
            break;
        case ZEND_TICKS:
            break;
        case ZEND_SEND_VAR_NO_REF:
            break;
        case ZEND_CATCH:
            break;
        case ZEND_THROW:
            break;
        case ZEND_FETCH_CLASS:
            break;
        case ZEND_CLONE:
            break;
        case ZEND_RETURN_BY_REF:
            break;
        case ZEND_INIT_METHOD_CALL:
            break;
        case ZEND_INIT_STATIC_METHOD_CALL:
            break;
        case ZEND_ISSET_ISEMPTY_VAR:
            break;
        case ZEND_ISSET_ISEMPTY_DIM_OBJ:
            break;
        case ZEND_SEND_VAL_EX:
            break;
        case ZEND_SEND_VAR:
            break;
        case ZEND_INIT_USER_CALL:
            break;
        case ZEND_SEND_ARRAY:
            break;
        case ZEND_SEND_USER:
            break;
        case ZEND_STRLEN:
            break;
        case ZEND_DEFINED:
            break;
        case ZEND_TYPE_CHECK:
            break;
        case ZEND_VERIFY_RETURN_TYPE:
            break;
        case ZEND_FE_RESET_RW:
            break;
        case ZEND_FE_FETCH_RW:
            break;
        case ZEND_FE_FREE:
            break;
        case ZEND_INIT_DYNAMIC_CALL:
            break;
        case ZEND_DO_ICALL:
            break;
        case ZEND_DO_UCALL:
            break;
        case ZEND_DO_FCALL_BY_NAME:
            break;
        case ZEND_PRE_INC_OBJ:
            break;
        case ZEND_PRE_DEC_OBJ:
            break;
        case ZEND_POST_INC_OBJ:
            break;
        case ZEND_POST_DEC_OBJ:
            break;
        case ZEND_ASSIGN_OBJ:
            break;
        case ZEND_OP_DATA:
            break;
        case ZEND_INSTANCEOF:
            break;
        case ZEND_DECLARE_CLASS:
            break;
        case ZEND_DECLARE_INHERITED_CLASS:
            break;
        case ZEND_DECLARE_FUNCTION:
            break;
        case ZEND_YIELD_FROM:
            break;
        case ZEND_DECLARE_CONST:
            break;
        case ZEND_ADD_INTERFACE:
            break;
        case ZEND_DECLARE_INHERITED_CLASS_DELAYED:
            break;
        case ZEND_VERIFY_ABSTRACT_CLASS:
            break;
        case ZEND_ASSIGN_DIM:
            break;
        case ZEND_ISSET_ISEMPTY_PROP_OBJ:
            break;
        case ZEND_HANDLE_EXCEPTION:
            break;
        case ZEND_USER_OPCODE:
            break;
        case ZEND_ASSERT_CHECK:
            break;
        case ZEND_JMP_SET:
            break;
        case ZEND_DECLARE_LAMBDA_FUNCTION:
            break;
        case ZEND_ADD_TRAIT:
            break;
        case ZEND_BIND_TRAITS:
            break;
        case ZEND_SEPARATE:
            break;
        case ZEND_FETCH_CLASS_NAME:
            break;
        case ZEND_CALL_TRAMPOLINE:
            break;
        case ZEND_DISCARD_EXCEPTION:
            break;
        case ZEND_YIELD:
            break;
        case ZEND_GENERATOR_RETURN:
            break;
        case ZEND_FAST_CALL:
            break;
        case ZEND_FAST_RET:
            break;
        case ZEND_RECV_VARIADIC:
            break;
        case ZEND_SEND_UNPACK:
            break;
        case ZEND_POW:
            break;
        case ZEND_ASSIGN_POW:
            break;
        case ZEND_BIND_GLOBAL:
            break;
        case ZEND_COALESCE:
            break;
        case ZEND_SPACESHIP:
            break;
        case ZEND_DECLARE_ANON_CLASS:
            break;
        case ZEND_DECLARE_ANON_INHERITED_CLASS:
            break;
        case ZEND_FETCH_STATIC_PROP_R:
            break;
        case ZEND_FETCH_STATIC_PROP_W:
            break;
        case ZEND_FETCH_STATIC_PROP_RW:
            break;
        case ZEND_FETCH_STATIC_PROP_IS:
            break;
        case ZEND_FETCH_STATIC_PROP_FUNC_ARG:
            break;
        case ZEND_FETCH_STATIC_PROP_UNSET:
            break;
        case ZEND_UNSET_STATIC_PROP:
            break;
        case ZEND_ISSET_ISEMPTY_STATIC_PROP:
            break;
        case ZEND_FETCH_CLASS_CONSTANT:
            break;
        case ZEND_BIND_LEXICAL:
            break;
        case ZEND_BIND_STATIC:
            break;
        case ZEND_FETCH_THIS:
            break;
        case ZEND_ISSET_ISEMPTY_THIS:
            break;
        case ZEND_SWITCH_LONG:
            break;
        case ZEND_SWITCH_STRING:
            break;
        case ZEND_IN_ARRAY:
            break;
        case ZEND_COUNT:
            break;
        case ZEND_GET_CLASS:
            break;
        case ZEND_GET_CALLED_CLASS:
            break;
        case ZEND_GET_TYPE:
            break;
        case ZEND_FUNC_NUM_ARGS:
            break;
        case ZEND_FUNC_GET_ARGS:
            break;
        case ZEND_UNSET_CV:
            break;
        case ZEND_ISSET_ISEMPTY_CV:
            break;
        default:
            break;
        }

        op++;
    }


	//函数结束之后添加return指令
	appendByte(databuf, JVM_OPC_return);
	appendChar(linebuf, code_start_pc);
	appendChar(linebuf, op_array->line_end);
	code_start_pc ++;

	end_attr(alen_idx);

	//todo try-catch编译
	appendChar(databuf, 0);

	//lineNumberTable 每条指令对应的源文件行号
	appendChar(databuf, 1);
	appendChar(databuf, appendUtf8(java_LineNumberTable));
	appendInt(databuf, linebuf->length + 2);
	if (linebuf->length > 0) {
		appendChar(databuf, linebuf->length / 4);
		appendBytes(databuf, linebuf->elems, linebuf->length);
	} else {
		appendChar(databuf, 0);
	}

	// 重置变量
	code_start_pc = 0;
}

static void write_literals(zend_op_array *op_array)
{
	int i;
	for (i = 0; i < op_array->last_literal; i ++) {
		zval *tmp = op_array->literals + i;

		switch (Z_TYPE_P(tmp))
		{
		case IS_STRING:
		case IS_LONG:
		case IS_DOUBLE:
			putValue(pool, tmp);
			break;
		default:
			break;
		}
	}

	zval *data;

	if (op_array->static_variables) {
		ZEND_HASH_FOREACH_VAL(op_array->static_variables, data) {
			switch (Z_TYPE_P(data))
			{
			case IS_STRING:
			case IS_LONG:	
			case IS_DOUBLE:
				putValue(pool, data);
				break;
			default:
				break;
			}
		} ZEND_HASH_FOREACH_END();
	}

}

static void do_transfer(char *filename, zend_op_array *op_array, int java_version)
{
    FILE *fp = fopen("./Test.class", "wb+");
    if (fp == NULL) {
        zend_error(E_ERROR, "can't open the file");
        return;
    }

	appendInt(poolbuf, JAVA_MAGIC_NUMBER);
	// todo 更换成enum类型定义java的版本
	appendChar(poolbuf, 0);
	appendChar(poolbuf, JAVA_SE_8_MAJOR);
	
	int pool_count_idx = poolbuf->length;
	appendChar(poolbuf, 0);

	init_java_klass_constant();

	//write_literals(op_array);	

	//databuf默认值初始化
	appendChar(databuf, JVM_ACC_PUBLIC|JVM_ACC_SUPER);
	appendChar(databuf, appendClass("Test"));
	appendChar(databuf, appendClass(java_ParentObjectStr));
	appendChar(databuf, 0);
	appendChar(databuf, 0);
	appendChar(databuf, 2);

	//初始化构造方法
	append_java_init_method();

	appendChar(databuf, JVM_ACC_PUBLIC|JVM_ACC_STATIC);
	appendChar(databuf, appendUtf8(java_MainFunc));
	appendChar(databuf, appendUtf8(java_MainRetSignture));
	appendChar(databuf, 1);

	appendChar(databuf, appendUtf8(java_Code));
	appendInt(databuf, 0); //attribute_length

	int alen_idx = databuf->length;

	process_op_array(op_array);

	end_attr(alen_idx);

	appendChar(databuf, 1);
	appendChar(databuf, appendUtf8(java_SourceFile));
	appendInt(databuf, 2);
	appendChar(databuf, appendUtf8("Test.java"));

	// 常量池的个数写入
	appendCharByPos(poolbuf, pool->last_var, pool_count_idx);

	appendBytes(poolbuf, databuf->elems, databuf->length);

	fwrite(poolbuf->elems, sizeof(char), poolbuf->length, fp);
	fclose(fp);
}

PHPAPI int transfer_to_java_file(zend_file_handle *primary_file, int java_version)
{

    int ret_val = SUCCESS;
    zend_try
    {

        VCWD_CHDIR_FILE(primary_file->filename);

        php2java_startup();

        zend_op_array *op_array;

        op_array = compile_file(primary_file, ZEND_REQUIRE);

        if (op_array)
        {

            char realfile[MAXPATHLEN];
            VCWD_REALPATH(primary_file->filename, realfile);

            primary_file->opened_path = zend_string_init(realfile, strlen(realfile), 0);
            zend_hash_add_empty_element(&EG(included_files), primary_file->opened_path);

            do_transfer(realfile, op_array, java_version);

            destroy_op_array(op_array);
            efree(op_array);
        }
        else
        {
            ret_val = FAILURE;
        }

        zend_destroy_file_handle(primary_file);
        php2java_shutdown();
    }
    zend_end_try();

    if (EG(exception))
    {
        zend_try
        {
            zend_exception_error(EG(exception), E_ERROR);
        }
        zend_end_try();
    }

    return ret_val;
}



/*************
 * 
 * 根据PHP语法树转化成java字节码
 * 
 *************/

/***
 * 方法定义
 ***/
void php2java_compile_expr(znode *result, zend_ast *ast);
void *php2java_hash_find_ptr_lc(HashTable *ht, const char *str, size_t len);
void php2java_compile_stmt(zend_ast *ast);
void php2java_compile_file(zend_file_handle *primary_file);


static zend_op *php2java_emit_op(znode *result, zend_uchar opcode, znode *op1, znode *op2) /* {{{ */
{
	zend_op *opline = get_next_op(CG(active_op_array));
	opline->opcode = opcode;

	// if (op1 == NULL) {
	// 	SET_UNUSED(opline->op1);
	// } else {
	// 	SET_NODE(opline->op1, op1);
	// }

	// if (op2 == NULL) {
	// 	SET_UNUSED(opline->op2);
	// } else {
	// 	SET_NODE(opline->op2, op2);
	// }

	// zend_check_live_ranges(opline);

	// if (result) {
	// 	zend_make_var_result(result, opline);
	// }
	return opline;
}

static zend_op *php2java_emit_op_tmp(znode *result, zend_uchar opcode, znode *op1, znode *op2) /* {{{ */
{
	// zend_op *opline = get_next_op(CG(active_op_array));
	// opline->opcode = opcode;

	// if (op1 == NULL) {
	// 	SET_UNUSED(opline->op1);
	// } else {
	// 	SET_NODE(opline->op1, op1);
	// }

	// if (op2 == NULL) {
	// 	SET_UNUSED(opline->op2);
	// } else {
	// 	SET_NODE(opline->op2, op2);
	// }

	// zend_check_live_ranges(opline);

	// if (result) {
	// 	zend_make_tmp_result(result, opline);
	// }

	// return opline;
}

static inline zend_op *php2java_delayed_emit_op(znode *result, zend_uchar opcode, znode *op1, znode *op2) /* {{{ */
{
	// zend_op tmp_opline;
	// init_op(&tmp_opline);
	// tmp_opline.opcode = opcode;
	// if (op1 == NULL) {
	// 	SET_UNUSED(tmp_opline.op1);
	// } else {
	// 	SET_NODE(tmp_opline.op1, op1);
	// }
	// if (op2 == NULL) {
	// 	SET_UNUSED(tmp_opline.op2);
	// } else {
	// 	SET_NODE(tmp_opline.op2, op2);
	// }
	// if (result) {
	// 	zend_make_var_result(result, &tmp_opline);
	// }

	// zend_stack_push(&CG(delayed_oplines_stack), &tmp_opline);
	// return zend_stack_top(&CG(delayed_oplines_stack));
}

static uint32_t get_temporary_variable(zend_op_array *op_array) /* {{{ */
{
	return (uint32_t)op_array->T++;
}

static void label_ptr_dtor(zval *zv) /* {{{ */
{
	efree_size(Z_PTR_P(zv), sizeof(zend_label));
}

static zend_bool is_this_fetch(zend_ast *ast) /* {{{ */
{
	if (ast->kind == ZEND_AST_VAR && ast->child[0]->kind == ZEND_AST_ZVAL) {
		zval *name = zend_ast_get_zval(ast->child[0]);
		return Z_TYPE_P(name) == IS_STRING && zend_string_equals_literal(Z_STR_P(name), "this");
	}

	return 0;
}

static inline zend_bool zend_is_variable(zend_ast *ast) /* {{{ */
{
	return ast->kind == ZEND_AST_VAR || ast->kind == ZEND_AST_DIM
		|| ast->kind == ZEND_AST_PROP || ast->kind == ZEND_AST_STATIC_PROP
		|| ast->kind == ZEND_AST_CALL || ast->kind == ZEND_AST_METHOD_CALL
		|| ast->kind == ZEND_AST_STATIC_CALL;
}

static inline zend_bool zend_is_call(zend_ast *ast) /* {{{ */
{
	return ast->kind == ZEND_AST_CALL
		|| ast->kind == ZEND_AST_METHOD_CALL
		|| ast->kind == ZEND_AST_STATIC_CALL;
}
/* }}} */

static inline zend_bool zend_is_unticked_stmt(zend_ast *ast) /* {{{ */
{
	return ast->kind == ZEND_AST_STMT_LIST || ast->kind == ZEND_AST_LABEL
		|| ast->kind == ZEND_AST_PROP_DECL || ast->kind == ZEND_AST_CLASS_CONST_DECL
		|| ast->kind == ZEND_AST_USE_TRAIT || ast->kind == ZEND_AST_METHOD;
}
/* }}} */

static inline zend_bool zend_can_write_to_variable(zend_ast *ast) /* {{{ */
{
	while (ast->kind == ZEND_AST_DIM || ast->kind == ZEND_AST_PROP) {
		ast = ast->child[0];
	}

	return zend_is_variable(ast);
}
/* }}} */

static void zend_adjust_for_fetch_type(zend_op *opline, uint32_t type) /* {{{ */
{
	zend_uchar factor = (opline->opcode == ZEND_FETCH_STATIC_PROP_R) ? 1 : 3;

	if (opline->opcode == ZEND_FETCH_THIS) {
		return;
	}

	switch (type & BP_VAR_MASK) {
		case BP_VAR_R:
			return;
		case BP_VAR_W:
			opline->opcode += 1 * factor;
			return;
		case BP_VAR_RW:
			opline->opcode += 2 * factor;
			return;
		case BP_VAR_IS:
			opline->opcode += 3 * factor;
			return;
		case BP_VAR_FUNC_ARG:
			opline->opcode += 4 * factor;
			opline->extended_value |= type >> BP_VAR_SHIFT;
			return;
		case BP_VAR_UNSET:
			opline->opcode += 5 * factor;
			return;
		EMPTY_SWITCH_DEFAULT_CASE()
	}
}
/* {{{ */

static zend_op *php2java_compile_simple_var_no_cv(znode *result, zend_ast *ast, uint32_t type, int delayed) /* {{{ */
{
	zend_ast *name_ast = ast->child[0];
	znode name_node;
	zend_op *opline;

	php2java_compile_expr(&name_node, name_ast);
	if (name_node.op_type == IS_CONST) {
		convert_to_string(&name_node.u.constant);
	}

	if (delayed) {
		opline = php2java_delayed_emit_op(result, ZEND_FETCH_R, &name_node, NULL);
	} else {
		opline = php2java_emit_op(result, ZEND_FETCH_R, &name_node, NULL);
	}

	if (name_node.op_type == IS_CONST
	    // zend_is_auto_global(Z_STR(name_node.u.constant))
        ) {

		opline->extended_value = ZEND_FETCH_GLOBAL;
	} else {
		opline->extended_value = ZEND_FETCH_LOCAL;
	}

	return opline;
}

static int php2java_try_compile_cv(znode *result, zend_ast *ast) /* {{{ */
{
	// zend_ast *name_ast = ast->child[0];
	// if (name_ast->kind == ZEND_AST_ZVAL) {
	// 	zend_string *name = zval_get_string(zend_ast_get_zval(name_ast));

	// 	if (zend_is_auto_global(name)) {
	// 		zend_string_release(name);
	// 		return FAILURE;
	// 	}

	// 	result->op_type = IS_CV;
	// 	result->u.op.var = lookup_cv(CG(active_op_array), name);

	// 	/* lookup_cv may be using another zend_string instance  */
	// 	name = CG(active_op_array)->vars[EX_VAR_TO_NUM(result->u.op.var)];

	// 	return SUCCESS;
	// }

	return FAILURE;
}

static void php2java_compile_simple_var(znode *result, zend_ast *ast, uint32_t type, int delayed) /* {{{ */
{
	zend_op *opline;

	if (is_this_fetch(ast)) {
		opline = php2java_emit_op(result, ZEND_FETCH_THIS, NULL, NULL);
		zend_adjust_for_fetch_type(opline, type);
	} else if (php2java_try_compile_cv(result, ast) == FAILURE) {
		zend_op *opline = php2java_compile_simple_var_no_cv(result, ast, type, delayed);
		zend_adjust_for_fetch_type(opline, type);
	}
}

static void php2java_register_seen_symbol(zend_string *name, uint32_t kind) {
	zval *zv = zend_hash_find(&FC(seen_symbols), name);
	if (zv) {
		Z_LVAL_P(zv) |= kind;
	} else {
		zval tmp;
		ZVAL_LONG(&tmp, kind);
		zend_hash_add_new(&FC(seen_symbols), name, &tmp);
	}
}

static zend_string *php2java_generate_anon_class_name(unsigned char *lex_pos) /* {{{ */
{
	zend_string *result;
	char char_pos_buf[32];
	size_t char_pos_len = zend_sprintf(char_pos_buf, "%p", lex_pos);
	zend_string *filename = CG(active_op_array)->filename;

	/* NULL, name length, filename length, last accepting char position length */
	result = zend_string_alloc(sizeof("class@anonymous") + ZSTR_LEN(filename) + char_pos_len, 0);
	sprintf(ZSTR_VAL(result), "class@anonymous%c%s%s", '\0', ZSTR_VAL(filename), char_pos_buf);
	return zend_new_interned_string(result);
}

static zend_constant *php2java_lookup_reserved_const(const char *name, size_t len) /* {{{ */
{
	zend_constant *c = php2java_hash_find_ptr_lc(EG(zend_constants), name, len);
	if (c && !(c->flags & CONST_CS) && (c->flags & CONST_CT_SUBST)) {
		return c;
	}
	return NULL;
}

static inline uint32_t php2java_emit_cond_jump(zend_uchar opcode, znode *cond, uint32_t opnum_target) /* {{{ */
{
	uint32_t opnum = get_next_op_number(CG(active_op_array));
	zend_op *opline;

	if ((cond->op_type & (IS_CV|IS_CONST))
	 && opnum > 0
	 && zend_is_smart_branch(CG(active_op_array)->opcodes + opnum - 1)) {
		/* emit extra NOP to avoid incorrect SMART_BRANCH in very rare cases */
		php2java_emit_op(NULL, ZEND_NOP, NULL, NULL);
		opnum = get_next_op_number(CG(active_op_array));
	}
	opline = php2java_emit_op(NULL, opcode, cond, NULL);
	opline->op2.opline_num = opnum_target;
	return opnum;
}

static inline uint32_t zend_emit_jump(uint32_t opnum_target) /* {{{ */
{
	uint32_t opnum = get_next_op_number(CG(active_op_array));
	zend_op *opline = php2java_emit_op(NULL, ZEND_JMP, NULL, NULL);
	opline->op1.opline_num = opnum_target;
	return opnum;
}

static zend_bool should_use_jumptable(zend_ast_list *cases, zend_uchar jumptable_type) {
	if (CG(compiler_options) & ZEND_COMPILE_NO_JUMPTABLES) {
		return 0;
	}

	/* Thresholds are chosen based on when the average switch time for equidistributed
	 * input becomes smaller when using the jumptable optimization. */
	if (jumptable_type == IS_LONG) {
		return cases->children >= 5;
	} else {
		ZEND_ASSERT(jumptable_type == IS_STRING);
		return cases->children >= 2;
	}
}

static uint32_t php2java_add_try_element(uint32_t try_op) /* {{{ */
{
	zend_op_array *op_array = CG(active_op_array);
	uint32_t try_catch_offset = op_array->last_try_catch++;
	zend_try_catch_element *elem;

	op_array->try_catch_array = safe_erealloc(
		op_array->try_catch_array, sizeof(zend_try_catch_element), op_array->last_try_catch, 0);

	elem = &op_array->try_catch_array[try_catch_offset];
	elem->try_op = try_op;
	elem->catch_op = 0;
	elem->finally_op = 0;
	elem->finally_end = 0;

	return try_catch_offset;
}

uint32_t php2java_get_class_fetch_type(zend_string *name) /* {{{ */
{
	if (zend_string_equals_literal_ci(name, "self")) {
		return ZEND_FETCH_CLASS_SELF;
	} else if (zend_string_equals_literal_ci(name, "parent")) {
		return ZEND_FETCH_CLASS_PARENT;
	} else if (zend_string_equals_literal_ci(name, "static")) {
		return ZEND_FETCH_CLASS_STATIC;
	} else {
		return ZEND_FETCH_CLASS_DEFAULT;
	}
}

static uint32_t php2java_get_class_fetch_type_ast(zend_ast *name_ast) /* {{{ */
{
	/* Fully qualified names are always default refs */
	if (name_ast->attr == ZEND_NAME_FQ) {
		return ZEND_FETCH_CLASS_DEFAULT;
	}

	return php2java_get_class_fetch_type(zend_ast_get_str(name_ast));
}

static inline zend_bool php2java_is_const_default_class_ref(zend_ast *name_ast) /* {{{ */
{
	if (name_ast->kind != ZEND_AST_ZVAL) {
		return 0;
	}

	return ZEND_FETCH_CLASS_DEFAULT == php2java_get_class_fetch_type_ast(name_ast);
}

static int php2java_declare_is_first_statement(zend_ast *ast) /* {{{ */
{
	uint32_t i = 0;
	zend_ast_list *file_ast = zend_ast_get_list(CG(ast));

	/* Check to see if this declare is preceeded only by declare statements */
	while (i < file_ast->children) {
		if (file_ast->child[i] == ast) {
			return SUCCESS;
		} else if (file_ast->child[i] == NULL) {
			/* Empty statements are not allowed prior to a declare */
			return FAILURE;
		} else if (file_ast->child[i]->kind != ZEND_AST_DECLARE) {
			/* declares can only be preceeded by other declares */
			return FAILURE;
		}
		i++;
	}
	return FAILURE;
}

static zend_string *zend_new_interned_string_safe(zend_string *str) /* {{{ */ {
	zend_string *interned_str;

	zend_string_addref(str);
	interned_str = zend_new_interned_string(str);
	if (str != interned_str) {
		return interned_str;
	} else {
		zend_string_release(str);
		return str;
	}
}

static zend_op *php2java_compile_class_ref(znode *result, zend_ast *name_ast, int throw_exception) /* {{{ */
{
	zend_op *opline;
	znode name_node;
	php2java_compile_expr(&name_node, name_ast);

	if (name_node.op_type == IS_CONST) {
		zend_string *name;
		uint32_t fetch_type;

		if (Z_TYPE(name_node.u.constant) != IS_STRING) {
			zend_error_noreturn(E_COMPILE_ERROR, "Illegal class name");
		}

		name = Z_STR(name_node.u.constant);
		fetch_type = php2java_get_class_fetch_type(name);

		opline = php2java_emit_op(result, ZEND_FETCH_CLASS, NULL, NULL);
		opline->extended_value = fetch_type | (throw_exception ? ZEND_FETCH_CLASS_EXCEPTION : 0);

		if (fetch_type == ZEND_FETCH_CLASS_DEFAULT) {
			uint32_t type = name_ast->kind == ZEND_AST_ZVAL ? name_ast->attr : ZEND_NAME_FQ;
			opline->op2_type = IS_CONST;
			// opline->op2.constant = zend_add_class_name_literal(CG(active_op_array),
			// 	zend_resolve_class_name(name, type));
		} else {
			// zend_ensure_valid_class_fetch_type(fetch_type);
		}

		zend_string_release(name);
	} else {
		opline = php2java_emit_op(result, ZEND_FETCH_CLASS, NULL, &name_node);
		opline->extended_value = ZEND_FETCH_CLASS_DEFAULT | (throw_exception ? ZEND_FETCH_CLASS_EXCEPTION : 0);
	}

	return opline;
}

void *php2java_hash_find_ptr_lc(HashTable *ht, const char *str, size_t len) {
	void *result;
	zend_string *lcname;
	ALLOCA_FLAG(use_heap);

	ZSTR_ALLOCA_ALLOC(lcname, len, use_heap);
	zend_str_tolower_copy(ZSTR_VAL(lcname), str, len);
	result = zend_hash_find_ptr(ht, lcname);
	ZSTR_ALLOCA_FREE(lcname, use_heap);

	return result;
}

void php2java_compile_dim(znode *result, zend_ast *ast, uint32_t type) /* {{{ */
{
	// zend_op *opline = zend_compile_dim_common(result, ast, type);
	// zend_adjust_for_fetch_type(opline, type);
}

void php2java_compile_prop(znode *result, zend_ast *ast, uint32_t type) /* {{{ */
{
	// zend_op *opline = zend_compile_prop_common(result, ast, type);
	// zend_adjust_for_fetch_type(opline, type);
}

void php2java_compile_static_prop(znode *result, zend_ast *ast, uint32_t type, int delayed) /* {{{ */
{
	// zend_op *opline = zend_compile_static_prop_common(result, ast, type, delayed);
	// zend_adjust_for_fetch_type(opline, type);
}

void php2java_compile_var(znode *result, zend_ast *ast, uint32_t type) /* {{{ */
{
	CG(zend_lineno) = zend_ast_get_lineno(ast);

	switch (ast->kind) {
		case ZEND_AST_VAR:
			php2java_compile_simple_var(result, ast, type, 0);
			return;
		case ZEND_AST_DIM:
			php2java_compile_dim(result, ast, type);
			return;
		case ZEND_AST_PROP:
			php2java_compile_prop(result, ast, type);
			return;
		case ZEND_AST_STATIC_PROP:
			php2java_compile_static_prop(result, ast, type, 0);
			return;
		case ZEND_AST_CALL:
			// zend_compile_call(result, ast, type);
			return;
		case ZEND_AST_METHOD_CALL:
			// zend_compile_method_call(result, ast, type);
			return;
		case ZEND_AST_STATIC_CALL:
			// zend_compile_static_call(result, ast, type);
			return;
		case ZEND_AST_ZNODE:
			*result = *zend_ast_get_znode(ast);
			return;
		default:
			if (type == BP_VAR_W || type == BP_VAR_RW || type == BP_VAR_UNSET) {
				zend_error_noreturn(E_COMPILE_ERROR,
					"Cannot use temporary expression in write context");
			}

			php2java_compile_expr(result, ast);
			return;
	}
}


void php2java_compile_assign(znode *result, zend_ast *ast) /* {{{ */
{
	zend_ast *var_ast = ast->child[0];
	zend_ast *expr_ast = ast->child[1];

	znode var_node, expr_node;
	zend_op *opline;
	uint32_t offset;

	// if (is_this_fetch(var_ast)) {
	// 	zend_error_noreturn(E_COMPILE_ERROR, "Cannot re-assign $this");
	// }

	// zend_ensure_writable_variable(var_ast);

	// switch (var_ast->kind) {
	// 	case ZEND_AST_VAR:
	// 	case ZEND_AST_STATIC_PROP:
	// 		offset = zend_delayed_compile_begin();
	// 		zend_delayed_compile_var(&var_node, var_ast, BP_VAR_W);
	// 		zend_compile_expr(&expr_node, expr_ast);
	// 		zend_delayed_compile_end(offset);
	// 		zend_emit_op(result, ZEND_ASSIGN, &var_node, &expr_node);
	// 		return;
	// 	case ZEND_AST_DIM:
	// 		offset = zend_delayed_compile_begin();
	// 		zend_delayed_compile_dim(result, var_ast, BP_VAR_W);

	// 		if (zend_is_assign_to_self(var_ast, expr_ast)
	// 		 && !is_this_fetch(expr_ast)) {
	// 			/* $a[0] = $a should evaluate the right $a first */
	// 			zend_compile_simple_var_no_cv(&expr_node, expr_ast, BP_VAR_R, 0);
	// 		} else {
	// 			zend_compile_expr(&expr_node, expr_ast);
	// 		}

	// 		opline = zend_delayed_compile_end(offset);
	// 		opline->opcode = ZEND_ASSIGN_DIM;

	// 		opline = zend_emit_op_data(&expr_node);
	// 		return;
	// 	case ZEND_AST_PROP:
	// 		offset = zend_delayed_compile_begin();
	// 		zend_delayed_compile_prop(result, var_ast, BP_VAR_W);
	// 		zend_compile_expr(&expr_node, expr_ast);

	// 		opline = zend_delayed_compile_end(offset);
	// 		opline->opcode = ZEND_ASSIGN_OBJ;

	// 		zend_emit_op_data(&expr_node);
	// 		return;
	// 	case ZEND_AST_ARRAY:
	// 		if (zend_list_has_assign_to_self(var_ast, expr_ast)) {
	// 			/* list($a, $b) = $a should evaluate the right $a first */
	// 			zend_compile_simple_var_no_cv(&expr_node, expr_ast, BP_VAR_R, 0);
	// 		} else {
	// 			zend_compile_expr(&expr_node, expr_ast);
	// 		}

	// 		zend_compile_list_assign(result, var_ast, &expr_node, var_ast->attr);
	// 		return;
	// 	EMPTY_SWITCH_DEFAULT_CASE();
	// }
}
/* }}} */

void php2java_compile_assign_ref(znode *result, zend_ast *ast) /* {{{ */
{
	zend_ast *target_ast = ast->child[0];
	zend_ast *source_ast = ast->child[1];

	znode target_node, source_node;
	zend_op *opline;
	uint32_t offset;

	if (is_this_fetch(target_ast)) {
		zend_error_noreturn(E_COMPILE_ERROR, "Cannot re-assign $this");
	}
	// zend_ensure_writable_variable(target_ast);

	// offset = zend_delayed_compile_begin();
	// zend_delayed_compile_var(&target_node, target_ast, BP_VAR_W);
	// zend_compile_var(&source_node, source_ast, BP_VAR_W);

	// if ((target_ast->kind != ZEND_AST_VAR
	//   || target_ast->child[0]->kind != ZEND_AST_ZVAL)
	//  && source_node.op_type != IS_CV) {
	// 	/* Both LHS and RHS expressions may modify the same data structure,
	// 	 * and the modification during RHS evaluation may dangle the pointer
	// 	 * to the result of the LHS evaluation.
	// 	 * Use MAKE_REF instruction to replace direct pointer with REFERENCE.
	// 	 * See: Bug #71539
	// 	 */
	// 	zend_emit_op(&source_node, ZEND_MAKE_REF, &source_node, NULL);
	// }

	// zend_delayed_compile_end(offset);

	// if (source_node.op_type != IS_VAR && zend_is_call(source_ast)) {
	// 	zend_error_noreturn(E_COMPILE_ERROR, "Cannot use result of built-in function in write context");
	// }

	// opline = zend_emit_op(result, ZEND_ASSIGN_REF, &target_node, &source_node);

	// if (zend_is_call(source_ast)) {
	// 	opline->extended_value = ZEND_RETURNS_FUNCTION;
	// }
}

void php2java_compile_new(znode *result, zend_ast *ast)
{

}

void php2java_compile_clone(znode *result, zend_ast *ast)
{

}

void phpj2ava_compile_compound_assign(znode *result, zend_ast *ast)
{

}

void php2java_compile_expr(znode *result, zend_ast *ast) /* {{{ */
{
	/* CG(zend_lineno) = ast->lineno; */
	CG(zend_lineno) = zend_ast_get_lineno(ast);

	switch (ast->kind) {
		case ZEND_AST_ZVAL:
			ZVAL_COPY(&result->u.constant, zend_ast_get_zval(ast));
			result->op_type = IS_CONST;
			return;
		case ZEND_AST_ZNODE:
			*result = *zend_ast_get_znode(ast);
			return;
		case ZEND_AST_VAR:
		case ZEND_AST_DIM:
		case ZEND_AST_PROP:
		case ZEND_AST_STATIC_PROP:
		case ZEND_AST_CALL:
		case ZEND_AST_METHOD_CALL:
		case ZEND_AST_STATIC_CALL:
			php2java_compile_var(result, ast, BP_VAR_R);
			return;
		case ZEND_AST_ASSIGN:
			php2java_compile_assign(result, ast);
			return;
		case ZEND_AST_ASSIGN_REF:
			php2java_compile_assign_ref(result, ast);
			return;
		case ZEND_AST_NEW:
			php2java_compile_new(result, ast);
			return;
		case ZEND_AST_CLONE:
			php2java_compile_clone(result, ast);
			return;
		case ZEND_AST_ASSIGN_OP:
			phpj2ava_compile_compound_assign(result, ast);
			return;
		case ZEND_AST_BINARY_OP:
			zend_compile_binary_op(result, ast);
			return;
		case ZEND_AST_GREATER:
		case ZEND_AST_GREATER_EQUAL:
			zend_compile_greater(result, ast);
			return;
		case ZEND_AST_UNARY_OP:
			zend_compile_unary_op(result, ast);
			return;
		case ZEND_AST_UNARY_PLUS:
		case ZEND_AST_UNARY_MINUS:
			zend_compile_unary_pm(result, ast);
			return;
		case ZEND_AST_AND:
		case ZEND_AST_OR:
			zend_compile_short_circuiting(result, ast);
			return;
		case ZEND_AST_POST_INC:
		case ZEND_AST_POST_DEC:
			zend_compile_post_incdec(result, ast);
			return;
		case ZEND_AST_PRE_INC:
		case ZEND_AST_PRE_DEC:
			zend_compile_pre_incdec(result, ast);
			return;
		case ZEND_AST_CAST:
			zend_compile_cast(result, ast);
			return;
		case ZEND_AST_CONDITIONAL:
			zend_compile_conditional(result, ast);
			return;
		case ZEND_AST_COALESCE:
			zend_compile_coalesce(result, ast);
			return;
		case ZEND_AST_PRINT:
			zend_compile_print(result, ast);
			return;
		case ZEND_AST_EXIT:
			zend_compile_exit(result, ast);
			return;
		case ZEND_AST_YIELD:
			zend_compile_yield(result, ast);
			return;
		case ZEND_AST_YIELD_FROM:
			zend_compile_yield_from(result, ast);
			return;
		case ZEND_AST_INSTANCEOF:
			zend_compile_instanceof(result, ast);
			return;
		case ZEND_AST_INCLUDE_OR_EVAL:
			//todo 此过程调用php2java_compile_file() 函数，参考 zend_include_or_eval 方法实现逻辑
			zend_compile_include_or_eval(result, ast);
			return;
		case ZEND_AST_ISSET:
		case ZEND_AST_EMPTY:
			zend_compile_isset_or_empty(result, ast);
			return;
		case ZEND_AST_SILENCE:
			zend_compile_silence(result, ast);
			return;
		case ZEND_AST_SHELL_EXEC:
			// zend_compile_shell_exec(result, ast);
			return;
		case ZEND_AST_ARRAY:
			zend_compile_array(result, ast);
			return;
		case ZEND_AST_CONST:
			zend_compile_const(result, ast);
			return;
		case ZEND_AST_CLASS_CONST:
			zend_compile_class_const(result, ast);
			return;
		case ZEND_AST_ENCAPS_LIST:
			// zend_compile_encaps_list(result, ast);
			return;
		case ZEND_AST_MAGIC_CONST:
			zend_compile_magic_const(result, ast);
			return;
		case ZEND_AST_CLOSURE:
			zend_compile_func_decl(result, ast);
			return;
		default:
			ZEND_ASSERT(0 /* not supported */);
	}
}

void php2java_compile_params(zend_ast *ast, zend_ast *return_type_ast) /* {{{ */
{
	zend_ast_list *list = zend_ast_get_list(ast);
	uint32_t i;
	zend_op_array *op_array = CG(active_op_array);
	zend_arg_info *arg_infos;

	if (return_type_ast) {
		zend_bool allow_null = 0;

		/* Use op_array->arg_info[-1] for return type */
		arg_infos = safe_emalloc(sizeof(zend_arg_info), list->children + 1, 0);
		arg_infos->name = NULL;
		arg_infos->pass_by_reference = (op_array->fn_flags & ZEND_ACC_RETURN_REFERENCE) != 0;
		arg_infos->is_variadic = 0;
		arg_infos->type = 0;

		if (return_type_ast->attr & ZEND_TYPE_NULLABLE) {
			allow_null = 1;
			return_type_ast->attr &= ~ZEND_TYPE_NULLABLE;
		}

		// zend_compile_typename(return_type_ast, arg_infos, allow_null);

		if (ZEND_TYPE_CODE(arg_infos->type) == IS_VOID && ZEND_TYPE_ALLOW_NULL(arg_infos->type)) {
			zend_error_noreturn(E_COMPILE_ERROR, "Void type cannot be nullable");
		}

		arg_infos++;
		op_array->fn_flags |= ZEND_ACC_HAS_RETURN_TYPE;
	} else {
		if (list->children == 0) {
			return;
		}
		arg_infos = safe_emalloc(sizeof(zend_arg_info), list->children, 0);
	}

	for (i = 0; i < list->children; ++i) {
		zend_ast *param_ast = list->child[i];
		zend_ast *type_ast = param_ast->child[0];
		zend_ast *var_ast = param_ast->child[1];
		zend_ast *default_ast = param_ast->child[2];
		zend_string *name = zend_ast_get_str(var_ast);
		zend_bool is_ref = (param_ast->attr & ZEND_PARAM_REF) != 0;
		zend_bool is_variadic = (param_ast->attr & ZEND_PARAM_VARIADIC) != 0;

		znode var_node, default_node;
		zend_uchar opcode;
		zend_op *opline;
		zend_arg_info *arg_info;

		// if (zend_is_auto_global(name)) {
		// 	zend_error_noreturn(E_COMPILE_ERROR, "Cannot re-assign auto-global variable %s",
		// 		ZSTR_VAL(name));
		// }

		var_node.op_type = IS_CV;
		// var_node.u.op.var = lookup_cv(CG(active_op_array), zend_string_copy(name));

		if (EX_VAR_TO_NUM(var_node.u.op.var) != i) {
			zend_error_noreturn(E_COMPILE_ERROR, "Redefinition of parameter $%s",
				ZSTR_VAL(name));
		} else if (zend_string_equals_literal(name, "this")) {
			zend_error_noreturn(E_COMPILE_ERROR, "Cannot use $this as parameter");
		}

		if (op_array->fn_flags & ZEND_ACC_VARIADIC) {
			zend_error_noreturn(E_COMPILE_ERROR, "Only the last parameter can be variadic");
		}

		if (is_variadic) {
			opcode = ZEND_RECV_VARIADIC;
			default_node.op_type = IS_UNUSED;
			op_array->fn_flags |= ZEND_ACC_VARIADIC;

			if (default_ast) {
				zend_error_noreturn(E_COMPILE_ERROR,
					"Variadic parameter cannot have a default value");
			}
		} else if (default_ast) {
			/* we cannot substitute constants here or it will break ReflectionParameter::getDefaultValueConstantName() and ReflectionParameter::isDefaultValueConstant() */
			uint32_t cops = CG(compiler_options);
			CG(compiler_options) |= ZEND_COMPILE_NO_CONSTANT_SUBSTITUTION | ZEND_COMPILE_NO_PERSISTENT_CONSTANT_SUBSTITUTION;
			opcode = ZEND_RECV_INIT;
			default_node.op_type = IS_CONST;
			// zend_const_expr_to_zval(&default_node.u.constant, default_ast);
			CG(compiler_options) = cops;
		} else {
			opcode = ZEND_RECV;
			default_node.op_type = IS_UNUSED;
			op_array->required_num_args = i + 1;
		}

		opline = php2java_emit_op(NULL, opcode, NULL, &default_node);
		// SET_NODE(opline->result, &var_node);
		opline->op1.num = i + 1;

		arg_info = &arg_infos[i];
		arg_info->name = zend_string_copy(name);
		arg_info->pass_by_reference = is_ref;
		arg_info->is_variadic = is_variadic;
		/* TODO: Keep compatibility, but may be better reset "allow_null" ??? */
		arg_info->type = ZEND_TYPE_ENCODE(0, 1);

		if (type_ast) {
			zend_bool allow_null;
			zend_bool has_null_default = default_ast
				&& (Z_TYPE(default_node.u.constant) == IS_NULL
					|| (Z_TYPE(default_node.u.constant) == IS_CONSTANT
						&& strcasecmp(Z_STRVAL(default_node.u.constant), "NULL") == 0));
			zend_bool is_explicitly_nullable = (type_ast->attr & ZEND_TYPE_NULLABLE) == ZEND_TYPE_NULLABLE;

			op_array->fn_flags |= ZEND_ACC_HAS_TYPE_HINTS;
			allow_null = has_null_default || is_explicitly_nullable;

			type_ast->attr &= ~ZEND_TYPE_NULLABLE;
			// zend_compile_typename(type_ast, arg_info, allow_null);

			if (ZEND_TYPE_CODE(arg_info->type) == IS_VOID) {
				zend_error_noreturn(E_COMPILE_ERROR, "void cannot be used as a parameter type");
			}

			if (type_ast->kind == ZEND_AST_TYPE) {
				if (ZEND_TYPE_CODE(arg_info->type) == IS_ARRAY) {
					if (default_ast && !has_null_default
						&& Z_TYPE(default_node.u.constant) != IS_ARRAY
						&& !Z_CONSTANT(default_node.u.constant)
					) {
						zend_error_noreturn(E_COMPILE_ERROR, "Default value for parameters "
							"with array type can only be an array or NULL");
					}
				} else if (ZEND_TYPE_CODE(arg_info->type) == IS_CALLABLE && default_ast) {
					if (!has_null_default && !Z_CONSTANT(default_node.u.constant)) {
						zend_error_noreturn(E_COMPILE_ERROR, "Default value for parameters "
							"with callable type can only be NULL");
					}
				}
			} else {
				if (default_ast && !has_null_default && !Z_CONSTANT(default_node.u.constant)) {
					if (ZEND_TYPE_IS_CLASS(arg_info->type)) {
						zend_error_noreturn(E_COMPILE_ERROR, "Default value for parameters "
							"with a class type can only be NULL");
					} else switch (ZEND_TYPE_CODE(arg_info->type)) {
						case IS_DOUBLE:
							if (Z_TYPE(default_node.u.constant) != IS_DOUBLE && Z_TYPE(default_node.u.constant) != IS_LONG) {
								zend_error_noreturn(E_COMPILE_ERROR, "Default value for parameters "
									"with a float type can only be float, integer, or NULL");
							}
							break;

						case IS_ITERABLE:
							if (Z_TYPE(default_node.u.constant) != IS_ARRAY) {
								zend_error_noreturn(E_COMPILE_ERROR, "Default value for parameters "
									"with iterable type can only be an array or NULL");
							}
							break;

						case IS_OBJECT:
							zend_error_noreturn(E_COMPILE_ERROR, "Default value for parameters "
								"with an object type can only be NULL");
							break;

						default:
							if (!ZEND_SAME_FAKE_TYPE(ZEND_TYPE_CODE(arg_info->type), Z_TYPE(default_node.u.constant))) {
								zend_error_noreturn(E_COMPILE_ERROR, "Default value for parameters "
									"with a %s type can only be %s or NULL",
									zend_get_type_by_const(ZEND_TYPE_CODE(arg_info->type)), zend_get_type_by_const(ZEND_TYPE_CODE(arg_info->type)));
							}
							break;
					}
				}
			}

			/* Allocate cache slot to speed-up run-time class resolution */
			if (opline->opcode == ZEND_RECV_INIT) {
				if (ZEND_TYPE_IS_CLASS(arg_info->type)) {
					// zend_alloc_cache_slot(opline->op2.constant);
				} else {
					Z_CACHE_SLOT(op_array->literals[opline->op2.constant]) = -1;
				}
			} else {
				if (ZEND_TYPE_IS_CLASS(arg_info->type)) {
					opline->op2.num = op_array->cache_size;
					op_array->cache_size += sizeof(void*);
				} else {
					opline->op2.num = -1;
				}
			}
		} else {
			if (opline->opcode == ZEND_RECV_INIT) {
				Z_CACHE_SLOT(op_array->literals[opline->op2.constant]) = -1;
			} else {
				opline->op2.num = -1;
			}
		}
	}

	/* These are assigned at the end to avoid unitialized memory in case of an error */
	op_array->num_args = list->children;
	op_array->arg_info = arg_infos;

	/* Don't count the variadic argument */
	if (op_array->fn_flags & ZEND_ACC_VARIADIC) {
		op_array->num_args--;
	}
	// zend_set_function_arg_flags((zend_function*)op_array);
}

void php2java_compile_static_var_common(zend_ast *var_ast, zval *value, zend_bool by_ref) /* {{{ */
{
	/**
	 * @brief java static 和PHP static 变量的区别
	 * java只能在类的属性上使用，不能在方法中使用
	 * php可以在累的属性上使用，也可以在方法或者函数上是使用
	 */
	znode var_node;
	php2java_compile_expr(&var_node, var_ast);

	appendByte(poolbuf, JVM_CONSTANT_Utf8);	
	appendChar(poolbuf, Z_STRLEN(var_node.u.constant));
	appendBytes(poolbuf, Z_STRVAL(var_node.u.constant), Z_STRLEN(var_node.u.constant));

	putValue(pool, &var_node.u.constant);
	if (Z_TYPE_P(value) != IS_NULL) {
		putValue(pool, value);
	}

	if (Z_TYPE_P(value) == IS_STRING) {
		appendByte(poolbuf, JVM_CONSTANT_Utf8);
		appendChar(poolbuf, Z_STRLEN_P(value));
		appendBytes(poolbuf, Z_STRVAL_P(value), Z_STRLEN_P(value));
	} else if (Z_TYPE_P(value) == IS_DOUBLE) {
		appendByte(poolbuf, JVM_CONSTANT_Double);
		appendDouble(poolbuf, Z_DVAL_P(value));	
	} else if (Z_TYPE_P(value) == IS_LONG) {
		//todo Java的整数赋值跟数大小有关系，待修改
		// appendByte(poolbuf, JVM_CONSTANT_Long);
		// appendLong(poolbuf, (long)(Z_LVAL_P(value)));
	}

	// if (!CG(active_op_array)->static_variables) {
	// 	if (CG(active_op_array)->scope) {
	// 		CG(active_op_array)->scope->ce_flags |= ZEND_HAS_STATIC_IN_METHODS;
	// 	}
	// 	ALLOC_HASHTABLE(CG(active_op_array)->static_variables);
	// 	zend_hash_init(CG(active_op_array)->static_variables, 8, NULL, ZVAL_PTR_DTOR, 0);
	// }

	// if (GC_REFCOUNT(CG(active_op_array)->static_variables) > 1) {
	// 	if (!(GC_FLAGS(CG(active_op_array)->static_variables) & IS_ARRAY_IMMUTABLE)) {
	// 		GC_REFCOUNT(CG(active_op_array)->static_variables)--;
	// 	}
	// 	CG(active_op_array)->static_variables = zend_array_dup(CG(active_op_array)->static_variables);
	// }
	// zend_hash_update(CG(active_op_array)->static_variables, Z_STR(var_node.u.constant), value);

	if (zend_string_equals_literal(Z_STR(var_node.u.constant), "this")) {
		zend_error_noreturn(E_COMPILE_ERROR, "Cannot use $this as static variable");
	}

	// opline = php2java_emit_op(NULL, ZEND_BIND_STATIC, NULL, &var_node);
	// opline->op1_type = IS_CV;
	// opline->op1.var = lookup_cv(CG(active_op_array), zend_string_copy(Z_STR(var_node.u.constant)));
	// opline->extended_value = by_ref;
}

void php2java_compile_stmt_list(zend_ast *ast) /* {{{ */
{
	zend_ast_list *list = zend_ast_get_list(ast);
	uint32_t i;
	for (i = 0; i < list->children; ++i) {
		php2java_compile_stmt(list->child[i]);
	}
}

void php2java_compile_static_var(zend_ast *ast) /* {{{ */
{
	zend_ast *var_ast = ast->child[0];
	zend_ast *value_ast = ast->child[1];
	zval value_zv;

	if (value_ast) {
		zend_const_expr_to_zval(&value_zv, value_ast);
	} else {
		ZVAL_NULL(&value_zv);
	}

	php2java_compile_static_var_common(var_ast, &value_zv, 1);
}

void php2java_compile_return(zend_ast *ast) /* {{{ */
{
	zend_ast *expr_ast = ast->child[0];
	zend_bool is_generator = (CG(active_op_array)->fn_flags & ZEND_ACC_GENERATOR) != 0;
	zend_bool by_ref = (CG(active_op_array)->fn_flags & ZEND_ACC_RETURN_REFERENCE) != 0;

	znode expr_node;
	zend_op *opline;

	if (is_generator) {
		/* For generators the by-ref flag refers to yields, not returns */
		by_ref = 0;
	}

	if (!expr_ast) {
		expr_node.op_type = IS_CONST;
		ZVAL_NULL(&expr_node.u.constant);
	} else if (by_ref && zend_is_variable(expr_ast) && !zend_is_call(expr_ast)) {
		php2java_compile_var(&expr_node, expr_ast, BP_VAR_W);
	} else {
		php2java_compile_expr(&expr_node, expr_ast);
	}

	// if ((CG(active_op_array)->fn_flags & ZEND_ACC_HAS_FINALLY_BLOCK)
	//  && (expr_node.op_type == IS_CV || (by_ref && expr_node.op_type == IS_VAR))
	//  && zend_has_finally()) {
	// 	/* Copy return value into temporary VAR to avoid modification in finally code */
	// 	if (by_ref) {
	// 		php2java_emit_op(&expr_node, ZEND_MAKE_REF, &expr_node, NULL);
	// 	} else {
	// 		php2java_emit_op_tmp(&expr_node, ZEND_QM_ASSIGN, &expr_node, NULL);
	// 	}
	// }

	/* Generator return types are handled separately */
	// if (!is_generator && CG(active_op_array)->fn_flags & ZEND_ACC_HAS_RETURN_TYPE) {
	// 	zend_emit_return_type_check(
	// 		expr_ast ? &expr_node : NULL, CG(active_op_array)->arg_info - 1, 0);
	// }

	// zend_handle_loops_and_finally((expr_node.op_type & (IS_TMP_VAR | IS_VAR)) ? &expr_node : NULL);

	opline = php2java_emit_op(NULL, by_ref ? ZEND_RETURN_BY_REF : ZEND_RETURN,
		&expr_node, NULL);

	if (by_ref && expr_ast) {
		if (zend_is_call(expr_ast)) {
			opline->extended_value = ZEND_RETURNS_FUNCTION;
		} else if (!zend_is_variable(expr_ast)) {
			opline->extended_value = ZEND_RETURNS_VALUE;
		}
	}
}

void php2java_compile_echo(zend_ast *ast) /* {{{ */
{
	zend_op *opline;
	zend_ast *expr_ast = ast->child[0];

	znode expr_node;
	php2java_compile_expr(&expr_node, expr_ast);

	opline = php2java_emit_op(NULL, ZEND_ECHO, &expr_node, NULL);
	opline->extended_value = 0;
}

void php2java_compile_throw(zend_ast *ast) /* {{{ */
{
	zend_ast *expr_ast = ast->child[0];

	znode expr_node;
	php2java_compile_expr(&expr_node, expr_ast);

	php2java_emit_op(NULL, ZEND_THROW, &expr_node, NULL);
}

void php2java_compile_break_continue(zend_ast *ast) /* {{{ */
{
	zend_ast *depth_ast = ast->child[0];

	zend_op *opline;
	zend_long depth;

	ZEND_ASSERT(ast->kind == ZEND_AST_BREAK || ast->kind == ZEND_AST_CONTINUE);

	if (depth_ast) {
		zval *depth_zv;
		if (depth_ast->kind != ZEND_AST_ZVAL) {
			zend_error_noreturn(E_COMPILE_ERROR, "'%s' operator with non-constant operand "
				"is no longer supported", ast->kind == ZEND_AST_BREAK ? "break" : "continue");
		}

		depth_zv = zend_ast_get_zval(depth_ast);
		if (Z_TYPE_P(depth_zv) != IS_LONG || Z_LVAL_P(depth_zv) < 1) {
			zend_error_noreturn(E_COMPILE_ERROR, "'%s' operator accepts only positive numbers",
				ast->kind == ZEND_AST_BREAK ? "break" : "continue");
		}

		depth = Z_LVAL_P(depth_zv);
	} else {
		depth = 1;
	}

	// if (CG(context).current_brk_cont == -1) {
	// 	zend_error_noreturn(E_COMPILE_ERROR, "'%s' not in the 'loop' or 'switch' context",
	// 		ast->kind == ZEND_AST_BREAK ? "break" : "continue");
	// } else {
	// 	if (!zend_handle_loops_and_finally_ex(depth, NULL)) {
	// 		zend_error_noreturn(E_COMPILE_ERROR, "Cannot '%s' " ZEND_LONG_FMT " level%s",
	// 			ast->kind == ZEND_AST_BREAK ? "break" : "continue",
	// 			depth, depth == 1 ? "" : "s");
	// 	}
	// }
	opline = php2java_emit_op(NULL, ast->kind == ZEND_AST_BREAK ? ZEND_BRK : ZEND_CONT, NULL, NULL);
	opline->op1.num = CG(context).current_brk_cont;
	opline->op2.num = depth;
}

void php2java_compile_goto(zend_ast *ast) /* {{{ */
{
	zend_ast *label_ast = ast->child[0];
	znode label_node;
	zend_op *opline;
	// uint32_t opnum_start = get_next_op_number(CG(active_op_array));

	// zend_compile_expr(&label_node, label_ast);

	// /* Label resolution and unwinding adjustments happen in pass two. */
	// zend_handle_loops_and_finally(NULL);
	// opline = php2java_emit_op(NULL, ZEND_GOTO, NULL, &label_node);
	// opline->op1.num = get_next_op_number(CG(active_op_array)) - opnum_start - 1;
	// opline->extended_value = CG(context).current_brk_cont;
}

void php2java_compile_label(zend_ast *ast) /* {{{ */
{
	zend_string *label = zend_ast_get_str(ast->child[0]);
	zend_label dest;

	if (!CG(context).labels) {
		ALLOC_HASHTABLE(CG(context).labels);
		zend_hash_init(CG(context).labels, 8, NULL, label_ptr_dtor, 0);
	}

	dest.brk_cont = CG(context).current_brk_cont;
	dest.opline_num = get_next_op_number(CG(active_op_array));

	if (!zend_hash_add_mem(CG(context).labels, label, &dest, sizeof(zend_label))) {
		zend_error_noreturn(E_COMPILE_ERROR, "Label '%s' already defined", ZSTR_VAL(label));
	}
}

void php2java_compile_while(zend_ast *ast) /* {{{ */
{
	zend_ast *cond_ast = ast->child[0];
	zend_ast *stmt_ast = ast->child[1];
	znode cond_node;
	// uint32_t opnum_start, opnum_jmp, opnum_cond;

	// opnum_jmp = zend_emit_jump(0);

	// zend_begin_loop(ZEND_NOP, NULL);

	// opnum_start = get_next_op_number(CG(active_op_array));
	// zend_compile_stmt(stmt_ast);

	// opnum_cond = get_next_op_number(CG(active_op_array));
	// zend_update_jump_target(opnum_jmp, opnum_cond);
	// zend_compile_expr(&cond_node, cond_ast);

	// php2java_emit_cond_jump(ZEND_JMPNZ, &cond_node, opnum_start);

	// zend_end_loop(opnum_cond, NULL);
}

void php2java_compile_for(zend_ast *ast) /* {{{ */
{
	zend_ast *init_ast = ast->child[0];
	zend_ast *cond_ast = ast->child[1];
	zend_ast *loop_ast = ast->child[2];
	zend_ast *stmt_ast = ast->child[3];

	// znode result;
	// uint32_t opnum_start, opnum_jmp, opnum_loop;

	// zend_compile_expr_list(&result, init_ast);
	// zend_do_free(&result);

	// opnum_jmp = zend_emit_jump(0);

	// zend_begin_loop(ZEND_NOP, NULL);

	// opnum_start = get_next_op_number(CG(active_op_array));
	// zend_compile_stmt(stmt_ast);

	// opnum_loop = get_next_op_number(CG(active_op_array));
	// zend_compile_expr_list(&result, loop_ast);
	// zend_do_free(&result);

	// zend_update_jump_target_to_next(opnum_jmp);
	// zend_compile_expr_list(&result, cond_ast);
	// zend_do_extended_info();

	// php2java_emit_cond_jump(ZEND_JMPNZ, &result, opnum_start);

	// zend_end_loop(opnum_loop, NULL);
}

void php2java_compile_foreach(zend_ast *ast) /* {{{ */
{
	zend_ast *expr_ast = ast->child[0];
	zend_ast *value_ast = ast->child[1];
	zend_ast *key_ast = ast->child[2];
	zend_ast *stmt_ast = ast->child[3];
	zend_bool by_ref = value_ast->kind == ZEND_AST_REF;
	zend_bool is_variable = zend_is_variable(expr_ast) && !zend_is_call(expr_ast)
		&& zend_can_write_to_variable(expr_ast);

	// znode expr_node, reset_node, value_node, key_node;
	// zend_op *opline;
	// uint32_t opnum_reset, opnum_fetch;

	// if (key_ast) {
	// 	if (key_ast->kind == ZEND_AST_REF) {
	// 		zend_error_noreturn(E_COMPILE_ERROR, "Key element cannot be a reference");
	// 	}
	// 	if (key_ast->kind == ZEND_AST_ARRAY) {
	// 		zend_error_noreturn(E_COMPILE_ERROR, "Cannot use list as key element");
	// 	}
	// }

	// if (by_ref) {
	// 	value_ast = value_ast->child[0];
	// }

	// if (by_ref && is_variable) {
	// 	zend_compile_var(&expr_node, expr_ast, BP_VAR_W);
	// } else {
	// 	zend_compile_expr(&expr_node, expr_ast);
	// }

	// if (by_ref) {
	// 	zend_separate_if_call_and_write(&expr_node, expr_ast, BP_VAR_W);
	// }

	// opnum_reset = get_next_op_number(CG(active_op_array));
	// opline = php2java_emit_op(&reset_node, by_ref ? ZEND_FE_RESET_RW : ZEND_FE_RESET_R, &expr_node, NULL);

	// zend_begin_loop(ZEND_FE_FREE, &reset_node);

	// opnum_fetch = get_next_op_number(CG(active_op_array));
	// opline = php2java_emit_op(NULL, by_ref ? ZEND_FE_FETCH_RW : ZEND_FE_FETCH_R, &reset_node, NULL);

	// if (is_this_fetch(value_ast)) {
	// 	zend_error_noreturn(E_COMPILE_ERROR, "Cannot re-assign $this");
	// } else if (value_ast->kind == ZEND_AST_VAR &&
	//     php2java_try_compile_cv(&value_node, value_ast) == SUCCESS) {
	// 	SET_NODE(opline->op2, &value_node);
	// } else {
	// 	opline->op2_type = IS_VAR;
	// 	opline->op2.var = get_temporary_variable(CG(active_op_array));
		// GET_NODE(&value_node, opline->op2);
		// if (by_ref) {
		// 	zend_emit_assign_ref_znode(value_ast, &value_node);
		// } else {
		// 	zend_emit_assign_znode(value_ast, &value_node);
		// }
	// }

	// if (key_ast) {
	// 	opline = &CG(active_op_array)->opcodes[opnum_fetch];
	// 	zend_make_tmp_result(&key_node, opline);
	// 	zend_emit_assign_znode(key_ast, &key_node);
	// }

	// php2java_compile_stmt(stmt_ast);

	// /* Place JMP and FE_FREE on the line where foreach starts. It would be
	//  * better to use the end line, but this information is not available
	//  * currently. */
	// CG(zend_lineno) = ast->lineno;
	// zend_emit_jump(opnum_fetch);

	// opline = &CG(active_op_array)->opcodes[opnum_reset];
	// opline->op2.opline_num = get_next_op_number(CG(active_op_array));

	// opline = &CG(active_op_array)->opcodes[opnum_fetch];
	// opline->extended_value = get_next_op_number(CG(active_op_array));

	// zend_end_loop(opnum_fetch, &reset_node);

	// opline = php2java_emit_op(NULL, ZEND_FE_FREE, &reset_node, NULL);
}

void php2java_compile_if(zend_ast *ast) /* {{{ */
{
	zend_ast_list *list = zend_ast_get_list(ast);
	uint32_t i;
	uint32_t *jmp_opnums = NULL;

	if (list->children > 1) {
		jmp_opnums = safe_emalloc(sizeof(uint32_t), list->children - 1, 0);
	}

	for (i = 0; i < list->children; ++i) {
		zend_ast *elem_ast = list->child[i];
		zend_ast *cond_ast = elem_ast->child[0];
		zend_ast *stmt_ast = elem_ast->child[1];

		znode cond_node;
		uint32_t opnum_jmpz;
		if (cond_ast) {
			php2java_compile_expr(&cond_node, cond_ast);
			opnum_jmpz = php2java_emit_cond_jump(ZEND_JMPZ, &cond_node, 0);
		}

		php2java_compile_stmt(stmt_ast);

		if (i != list->children - 1) {
			jmp_opnums[i] = zend_emit_jump(0);
		}

		if (cond_ast) {
			// zend_update_jump_target_to_next(opnum_jmpz);
		}
	}

	if (list->children > 1) {
		for (i = 0; i < list->children - 1; ++i) {
			// zend_update_jump_target_to_next(jmp_opnums[i]);
		}
		efree(jmp_opnums);
	}
}

void php2java_compile_switch(zend_ast *ast) /* {{{ */
{
	zend_ast *expr_ast = ast->child[0];
	zend_ast_list *cases = zend_ast_get_list(ast->child[1]);

	uint32_t i;
	zend_bool has_default_case = 0;

	znode expr_node, case_node;
	zend_op *opline;
	uint32_t *jmpnz_opnums, opnum_default_jmp, opnum_switch;
	zend_uchar jumptable_type;
	HashTable *jumptable = NULL;

	php2java_compile_expr(&expr_node, expr_ast);

	// zend_begin_loop(ZEND_FREE, &expr_node);

	// case_node.op_type = IS_TMP_VAR;
	// case_node.u.op.var = get_temporary_variable(CG(active_op_array));

	// jumptable_type = determine_switch_jumptable_type(cases);
	// if (jumptable_type != IS_UNDEF && should_use_jumptable(cases, jumptable_type)) {
	// 	znode jumptable_op;

	// 	ALLOC_HASHTABLE(jumptable);
	// 	zend_hash_init(jumptable, cases->children, NULL, NULL, 0);
	// 	jumptable_op.op_type = IS_CONST;
	// 	ZVAL_ARR(&jumptable_op.u.constant, jumptable);

	// 	opline = php2java_emit_op(NULL,
	// 		jumptable_type == IS_LONG ? ZEND_SWITCH_LONG : ZEND_SWITCH_STRING,
	// 		&expr_node, &jumptable_op);
	// 	if (opline->op1_type == IS_CONST) {
	// 		zval_copy_ctor(CT_CONSTANT(opline->op1));
	// 	}
	// 	opnum_switch = opline - CG(active_op_array)->opcodes;
	// }

	// jmpnz_opnums = safe_emalloc(sizeof(uint32_t), cases->children, 0);
	// for (i = 0; i < cases->children; ++i) {
	// 	zend_ast *case_ast = cases->child[i];
	// 	zend_ast *cond_ast = case_ast->child[0];
	// 	znode cond_node;

	// 	if (!cond_ast) {
	// 		if (has_default_case) {
	// 			CG(zend_lineno) = case_ast->lineno;
	// 			zend_error_noreturn(E_COMPILE_ERROR,
	// 				"Switch statements may only contain one default clause");
	// 		}
	// 		has_default_case = 1;
	// 		continue;
	// 	}

	// 	php2java_compile_expr(&cond_node, cond_ast);

	// 	if (expr_node.op_type == IS_CONST
	// 		&& Z_TYPE(expr_node.u.constant) == IS_FALSE) {
	// 		jmpnz_opnums[i] = php2java_emit_cond_jump(ZEND_JMPZ, &cond_node, 0);
	// 	} else if (expr_node.op_type == IS_CONST
	// 		&& Z_TYPE(expr_node.u.constant) == IS_TRUE) {
	// 		jmpnz_opnums[i] = php2java_emit_cond_jump(ZEND_JMPNZ, &cond_node, 0);
	// 	} else {
	// 		opline = php2java_emit_op(NULL, ZEND_CASE, &expr_node, &cond_node);
	// 		// SET_NODE(opline->result, &case_node);
	// 		if (opline->op1_type == IS_CONST) {
	// 			zval_copy_ctor(CT_CONSTANT(opline->op1));
	// 		}

	// 		jmpnz_opnums[i] = php2java_emit_cond_jump(ZEND_JMPNZ, &case_node, 0);
	// 	}
	// }

	// opnum_default_jmp = zend_emit_jump(0);

	// for (i = 0; i < cases->children; ++i) {
	// 	zend_ast *case_ast = cases->child[i];
	// 	zend_ast *cond_ast = case_ast->child[0];
	// 	zend_ast *stmt_ast = case_ast->child[1];

	// 	if (cond_ast) {
	// 		zend_update_jump_target_to_next(jmpnz_opnums[i]);

	// 		if (jumptable) {
	// 			zval *cond_zv = zend_ast_get_zval(cond_ast);
	// 			zval jmp_target;
	// 			ZVAL_LONG(&jmp_target, get_next_op_number(CG(active_op_array)));

	// 			ZEND_ASSERT(Z_TYPE_P(cond_zv) == jumptable_type);
	// 			if (Z_TYPE_P(cond_zv) == IS_LONG) {
	// 				zend_hash_index_add(jumptable, Z_LVAL_P(cond_zv), &jmp_target);
	// 			} else {
	// 				ZEND_ASSERT(Z_TYPE_P(cond_zv) == IS_STRING);
	// 				zend_hash_add(jumptable, Z_STR_P(cond_zv), &jmp_target);
	// 			}
	// 		}
	// 	} else {
	// 		zend_update_jump_target_to_next(opnum_default_jmp);

	// 		if (jumptable) {
	// 			opline = &CG(active_op_array)->opcodes[opnum_switch];
	// 			opline->extended_value = get_next_op_number(CG(active_op_array));
	// 		}
	// 	}

	// 	zend_compile_stmt(stmt_ast);
	// }

	// if (!has_default_case) {
	// 	zend_update_jump_target_to_next(opnum_default_jmp);

	// 	if (jumptable) {
	// 		opline = &CG(active_op_array)->opcodes[opnum_switch];
	// 		opline->extended_value = get_next_op_number(CG(active_op_array));
	// 	}
	// }

	// zend_end_loop(get_next_op_number(CG(active_op_array)), &expr_node);

	// if (expr_node.op_type & (IS_VAR|IS_TMP_VAR)) {
	// 	/* don't use emit_op() to prevent automatic live-range construction */
	// 	opline = get_next_op(CG(active_op_array));
	// 	opline->opcode = ZEND_FREE;
	// 	SET_NODE(opline->op1, &expr_node);
	// 	SET_UNUSED(opline->op2);
	// } else if (expr_node.op_type == IS_CONST) {
	// 	zval_dtor(&expr_node.u.constant);
	// }

	// efree(jmpnz_opnums);
}

void php2java_compile_try(zend_ast *ast) /* {{{ */
{
	zend_ast *try_ast = ast->child[0];
	zend_ast_list *catches = zend_ast_get_list(ast->child[1]);
	zend_ast *finally_ast = ast->child[2];

	uint32_t i, j;
	zend_op *opline;
	uint32_t try_catch_offset;
	uint32_t *jmp_opnums = safe_emalloc(sizeof(uint32_t), catches->children, 0);
	uint32_t orig_fast_call_var = CG(context).fast_call_var;
	uint32_t orig_try_catch_offset = CG(context).try_catch_offset;

	if (catches->children == 0 && !finally_ast) {
		zend_error_noreturn(E_COMPILE_ERROR, "Cannot use try without catch or finally");
	}

	/* label: try { } must not be equal to try { label: } */
	if (CG(context).labels) {
		zend_label *label;
		ZEND_HASH_REVERSE_FOREACH_PTR(CG(context).labels, label) {
			if (label->opline_num == get_next_op_number(CG(active_op_array))) {
				php2java_emit_op(NULL, ZEND_NOP, NULL, NULL);
			}
			break;
		} ZEND_HASH_FOREACH_END();
	}

	try_catch_offset = php2java_add_try_element(get_next_op_number(CG(active_op_array)));

	if (finally_ast) {
		// zend_loop_var fast_call;
		// if (!(CG(active_op_array)->fn_flags & ZEND_ACC_HAS_FINALLY_BLOCK)) {
		// 	CG(active_op_array)->fn_flags |= ZEND_ACC_HAS_FINALLY_BLOCK;
		// }
		// CG(context).fast_call_var = get_temporary_variable(CG(active_op_array));

		// /* Push FAST_CALL on unwind stack */
		// fast_call.opcode = ZEND_FAST_CALL;
		// fast_call.var_type = IS_TMP_VAR;
		// fast_call.var_num = CG(context).fast_call_var;
		// fast_call.u.try_catch_offset = try_catch_offset;
		// zend_stack_push(&CG(loop_var_stack), &fast_call);
	}

	CG(context).try_catch_offset = try_catch_offset;

	zend_compile_stmt(try_ast);

	if (catches->children != 0) {
		jmp_opnums[0] = zend_emit_jump(0);
	}

	for (i = 0; i < catches->children; ++i) {
		zend_ast *catch_ast = catches->child[i];
		zend_ast_list *classes = zend_ast_get_list(catch_ast->child[0]);
		zend_ast *var_ast = catch_ast->child[1];
		zend_ast *stmt_ast = catch_ast->child[2];
		zval *var_name = zend_ast_get_zval(var_ast);
		zend_bool is_last_catch = (i + 1 == catches->children);

		uint32_t *jmp_multicatch = safe_emalloc(sizeof(uint32_t), classes->children - 1, 0);
		uint32_t opnum_catch;

		CG(zend_lineno) = catch_ast->lineno;

		for (j = 0; j < classes->children; j++) {

			zend_ast *class_ast = classes->child[j];
			zend_bool is_last_class = (j + 1 == classes->children);

			if (!php2java_is_const_default_class_ref(class_ast)) {
				zend_error_noreturn(E_COMPILE_ERROR, "Bad class name in the catch statement");
			}

			opnum_catch = get_next_op_number(CG(active_op_array));
			if (i == 0 && j == 0) {
				CG(active_op_array)->try_catch_array[try_catch_offset].catch_op = opnum_catch;
			}

			opline = get_next_op(CG(active_op_array));
			opline->opcode = ZEND_CATCH;
			opline->op1_type = IS_CONST;
			// opline->op1.constant = zend_add_class_name_literal(CG(active_op_array),
			// 		zend_resolve_class_name_ast(class_ast));

			if (zend_string_equals_literal(Z_STR_P(var_name), "this")) {
				zend_error_noreturn(E_COMPILE_ERROR, "Cannot re-assign $this");
			}

			opline->op2_type = IS_CV;
			// opline->op2.var = lookup_cv(CG(active_op_array), zend_string_copy(Z_STR_P(var_name)));

			opline->result.num = is_last_catch && is_last_class;

			if (!is_last_class) {
				jmp_multicatch[j] = zend_emit_jump(0);
				opline = &CG(active_op_array)->opcodes[opnum_catch];
				// opline->extended_value = get_next_op_number(CG(active_op_array));
			}
		}

		for (j = 0; j < classes->children - 1; j++) {
			// zend_update_jump_target_to_next(jmp_multicatch[j]);
		}

		efree(jmp_multicatch);

		php2java_compile_stmt(stmt_ast);

		if (!is_last_catch) {
			jmp_opnums[i + 1] = zend_emit_jump(0);
		}

		opline = &CG(active_op_array)->opcodes[opnum_catch];
		if (!is_last_catch) {
			opline->extended_value = get_next_op_number(CG(active_op_array));
		}
	}

	for (i = 0; i < catches->children; ++i) {
		// zend_update_jump_target_to_next(jmp_opnums[i]);
	}

	if (finally_ast) {
		uint32_t opnum_jmp = get_next_op_number(CG(active_op_array)) + 1;
		// zend_loop_var discard_exception;

		// /* Pop FAST_CALL from unwind stack */
		// zend_stack_del_top(&CG(loop_var_stack));

		// /* Push DISCARD_EXCEPTION on unwind stack */
		// discard_exception.opcode = ZEND_DISCARD_EXCEPTION;
		// discard_exception.var_type = IS_TMP_VAR;
		// discard_exception.var_num = CG(context).fast_call_var;
		// zend_stack_push(&CG(loop_var_stack), &discard_exception);

		CG(zend_lineno) = finally_ast->lineno;

		opline = php2java_emit_op(NULL, ZEND_FAST_CALL, NULL, NULL);
		opline->op1.num = try_catch_offset;
		opline->result_type = IS_TMP_VAR;
		opline->result.var = CG(context).fast_call_var;

		php2java_emit_op(NULL, ZEND_JMP, NULL, NULL);

		php2java_compile_stmt(finally_ast);

		CG(active_op_array)->try_catch_array[try_catch_offset].finally_op = opnum_jmp + 1;
		CG(active_op_array)->try_catch_array[try_catch_offset].finally_end
			= get_next_op_number(CG(active_op_array));

		opline = php2java_emit_op(NULL, ZEND_FAST_RET, NULL, NULL);
		opline->op1_type = IS_TMP_VAR;
		opline->op1.var = CG(context).fast_call_var;
		opline->op2.num = orig_try_catch_offset;

		// zend_update_jump_target_to_next(opnum_jmp);

		CG(context).fast_call_var = orig_fast_call_var;

		/* Pop DISCARD_EXCEPTION from unwind stack */
		zend_stack_del_top(&CG(loop_var_stack));
	}

	CG(context).try_catch_offset = orig_try_catch_offset;

}

void php2java_compile_declare(zend_ast *ast) /* {{{ */
{
	zend_ast_list *declares = zend_ast_get_list(ast->child[0]);
	zend_ast *stmt_ast = ast->child[1];
	zend_declarables orig_declarables = FC(declarables);
	uint32_t i;

	for (i = 0; i < declares->children; ++i) {
		zend_ast *declare_ast = declares->child[i];
		zend_ast *name_ast = declare_ast->child[0];
		zend_ast *value_ast = declare_ast->child[1];
		zend_string *name = zend_ast_get_str(name_ast);

		if (value_ast->kind != ZEND_AST_ZVAL) {
			zend_error_noreturn(E_COMPILE_ERROR, "declare(%s) value must be a literal", ZSTR_VAL(name));
		}

		if (zend_string_equals_literal_ci(name, "ticks")) {
			zval value_zv;
			zend_const_expr_to_zval(&value_zv, value_ast);
			FC(declarables).ticks = zval_get_long(&value_zv);
			zval_dtor(&value_zv);
		} else if (zend_string_equals_literal_ci(name, "encoding")) {

			if (FAILURE == php2java_declare_is_first_statement(ast)) {
				zend_error_noreturn(E_COMPILE_ERROR, "Encoding declaration pragma must be "
					"the very first statement in the script");
			}
		} else if (zend_string_equals_literal_ci(name, "strict_types")) {
			zval value_zv;

			if (FAILURE == php2java_declare_is_first_statement(ast)) {
				zend_error_noreturn(E_COMPILE_ERROR, "strict_types declaration must be "
					"the very first statement in the script");
			}

			if (ast->child[1] != NULL) {
				zend_error_noreturn(E_COMPILE_ERROR, "strict_types declaration must not "
					"use block mode");
			}

			// zend_const_expr_to_zval(&value_zv, value_ast);

			if (Z_TYPE(value_zv) != IS_LONG || (Z_LVAL(value_zv) != 0 && Z_LVAL(value_zv) != 1)) {
				zend_error_noreturn(E_COMPILE_ERROR, "strict_types declaration must have 0 or 1 as its value");
			}

			if (Z_LVAL(value_zv) == 1) {
				CG(active_op_array)->fn_flags |= ZEND_ACC_STRICT_TYPES;
			}

		} else {
			zend_error(E_COMPILE_WARNING, "Unsupported declare '%s'", ZSTR_VAL(name));
		}
	}

	if (stmt_ast) {
		php2java_compile_stmt(stmt_ast);

		FC(declarables) = orig_declarables;
	}
}

void php2java_compile_func_decl(znode *result, zend_ast *ast) /* {{{ */
{
	zend_ast_decl *decl = (zend_ast_decl *) ast;
	zend_ast *params_ast = decl->child[0];
	zend_ast *uses_ast = decl->child[1];
	zend_ast *stmt_ast = decl->child[2];
	zend_ast *return_type_ast = decl->child[3];
	zend_bool is_method = decl->kind == ZEND_AST_METHOD;

	zend_op_array *orig_op_array = CG(active_op_array);
	zend_op_array *op_array = zend_arena_alloc(&CG(arena), sizeof(zend_op_array));
	zend_oparray_context orig_oparray_context;

	init_op_array(op_array, ZEND_USER_FUNCTION, INITIAL_OP_ARRAY_SIZE);

	op_array->fn_flags |= (orig_op_array->fn_flags & ZEND_ACC_STRICT_TYPES);
	op_array->fn_flags |= decl->flags;
	op_array->line_start = decl->start_lineno;
	op_array->line_end = decl->end_lineno;
	if (decl->doc_comment) {
		op_array->doc_comment = zend_string_copy(decl->doc_comment);
	}
	if (decl->kind == ZEND_AST_CLOSURE) {
		op_array->fn_flags |= ZEND_ACC_CLOSURE;
	}

	if (is_method) {
		zend_bool has_body = stmt_ast != NULL;
		zend_begin_method_decl(op_array, decl->name, has_body);
	} else {
		// zend_begin_func_decl(result, op_array, decl);
		// if (uses_ast) {
		// 	zend_compile_closure_binding(result, uses_ast);
		// }
	}

	CG(active_op_array) = op_array;

	zend_oparray_context_begin(&orig_oparray_context);

	if (CG(compiler_options) & ZEND_COMPILE_EXTENDED_INFO) {
		zend_op *opline_ext = php2java_emit_op(NULL, ZEND_EXT_NOP, NULL, NULL);
		opline_ext->lineno = decl->start_lineno;
	}

	{
		/* Push a separator to the loop variable stack */
		// zend_loop_var dummy_var;
		// dummy_var.opcode = ZEND_RETURN;

		// zend_stack_push(&CG(loop_var_stack), (void *) &dummy_var);
	}

	php2java_compile_params(params_ast, return_type_ast);
	if (CG(active_op_array)->fn_flags & ZEND_ACC_GENERATOR) {
		// zend_mark_function_as_generator();
		php2java_emit_op(NULL, ZEND_GENERATOR_CREATE, NULL, NULL);
	}
	if (uses_ast) {
		// zend_compile_closure_uses(uses_ast);
	}
	php2java_compile_stmt(stmt_ast);

	if (is_method) {
		// zend_check_magic_method_implementation(
		// 	CG(active_class_entry), (zend_function *) op_array, E_COMPILE_ERROR);
	}

	/* put the implicit return on the really last line */
	CG(zend_lineno) = decl->end_lineno;

	// zend_do_extended_info();
	// zend_emit_final_return(0);

	pass_two(CG(active_op_array));
	// zend_oparray_context_end(&orig_oparray_context);

	/* Pop the loop variable stack separator */
	zend_stack_del_top(&CG(loop_var_stack));

	CG(active_op_array) = orig_op_array;
}

void php2java_compile_prop_decl(zend_ast *ast) /* {{{ */
{
	zend_ast_list *list = zend_ast_get_list(ast);
	uint32_t flags = list->attr;
	zend_class_entry *ce = CG(active_class_entry);
	uint32_t i, children = list->children;

	if (ce->ce_flags & ZEND_ACC_INTERFACE) {
		zend_error_noreturn(E_COMPILE_ERROR, "Interfaces may not include member variables");
	}

	if (flags & ZEND_ACC_ABSTRACT) {
		zend_error_noreturn(E_COMPILE_ERROR, "Properties cannot be declared abstract");
	}

	for (i = 0; i < children; ++i) {
		zend_ast *prop_ast = list->child[i];
		zend_ast *name_ast = prop_ast->child[0];
		zend_ast *value_ast = prop_ast->child[1];
		zend_ast *doc_comment_ast = prop_ast->child[2];
		zend_string *name = zend_ast_get_str(name_ast);
		zend_string *doc_comment = NULL;
		zval value_zv;

		/* Doc comment has been appended as last element in ZEND_AST_PROP_ELEM ast */
		if (doc_comment_ast) {
			doc_comment = zend_string_copy(zend_ast_get_str(doc_comment_ast));
		}

		if (flags & ZEND_ACC_FINAL) {
			zend_error_noreturn(E_COMPILE_ERROR, "Cannot declare property %s::$%s final, "
				"the final modifier is allowed only for methods and classes",
				ZSTR_VAL(ce->name), ZSTR_VAL(name));
		}

		if (zend_hash_exists(&ce->properties_info, name)) {
			zend_error_noreturn(E_COMPILE_ERROR, "Cannot redeclare %s::$%s",
				ZSTR_VAL(ce->name), ZSTR_VAL(name));
		}

		if (value_ast) {
			zend_const_expr_to_zval(&value_zv, value_ast);
		} else {
			ZVAL_NULL(&value_zv);
		}

		name = zend_new_interned_string_safe(name);
		zend_declare_property_ex(ce, name, &value_zv, flags, doc_comment);
	}
}

void php2java_compile_class_const_decl(zend_ast *ast) /* {{{ */
{
	zend_ast_list *list = zend_ast_get_list(ast);
	zend_class_entry *ce = CG(active_class_entry);
	uint32_t i;

	if ((ce->ce_flags & ZEND_ACC_TRAIT) != 0) {
		zend_error_noreturn(E_COMPILE_ERROR, "Traits cannot have constants");
		return;
	}

	for (i = 0; i < list->children; ++i) {
		zend_ast *const_ast = list->child[i];
		zend_ast *name_ast = const_ast->child[0];
		zend_ast *value_ast = const_ast->child[1];
		zend_ast *doc_comment_ast = const_ast->child[2];
		zend_string *name = zend_ast_get_str(name_ast);
		zend_string *doc_comment = doc_comment_ast ? zend_string_copy(zend_ast_get_str(doc_comment_ast)) : NULL;
		zval value_zv;

		if (UNEXPECTED(ast->attr & (ZEND_ACC_STATIC|ZEND_ACC_ABSTRACT|ZEND_ACC_FINAL))) {
			if (ast->attr & ZEND_ACC_STATIC) {
				zend_error_noreturn(E_COMPILE_ERROR, "Cannot use 'static' as constant modifier");
			} else if (ast->attr & ZEND_ACC_ABSTRACT) {
				zend_error_noreturn(E_COMPILE_ERROR, "Cannot use 'abstract' as constant modifier");
			} else if (ast->attr & ZEND_ACC_FINAL) {
				zend_error_noreturn(E_COMPILE_ERROR, "Cannot use 'final' as constant modifier");
			}
		}

		zend_const_expr_to_zval(&value_zv, value_ast);

		name = zend_new_interned_string_safe(name);
		zend_declare_class_constant_ex(ce, name, &value_zv, ast->attr, doc_comment);
	}
}

void php2java_compile_use_trait(zend_ast *ast) /* {{{ */
{
	zend_ast_list *traits = zend_ast_get_list(ast->child[0]);
	zend_ast_list *adaptations = ast->child[1] ? zend_ast_get_list(ast->child[1]) : NULL;
	zend_class_entry *ce = CG(active_class_entry);
	// zend_op *opline;
	// uint32_t i;

	// for (i = 0; i < traits->children; ++i) {
	// 	zend_ast *trait_ast = traits->child[i];
	// 	zend_string *name = zend_ast_get_str(trait_ast);

	// 	if (ce->ce_flags & ZEND_ACC_INTERFACE) {
	// 		zend_error_noreturn(E_COMPILE_ERROR, "Cannot use traits inside of interfaces. "
	// 			"%s is used in %s", ZSTR_VAL(name), ZSTR_VAL(ce->name));
	// 	}

	// 	switch (php2java_get_class_fetch_type(name)) {
	// 		case ZEND_FETCH_CLASS_SELF:
	// 		case ZEND_FETCH_CLASS_PARENT:
	// 		case ZEND_FETCH_CLASS_STATIC:
	// 			zend_error_noreturn(E_COMPILE_ERROR, "Cannot use '%s' as trait name "
	// 				"as it is reserved", ZSTR_VAL(name));
	// 			break;
	// 	}

	// 	opline = get_next_op(CG(active_op_array));
	// 	opline->opcode = ZEND_ADD_TRAIT;
	// 	SET_NODE(opline->op1, &FC(implementing_class));
	// 	opline->op2_type = IS_CONST;
	// 	opline->op2.constant = zend_add_class_name_literal(CG(active_op_array),
	// 		zend_resolve_class_name_ast(trait_ast));

	// 	ce->num_traits++;
	// }

	// if (!adaptations) {
	// 	return;
	// }

	// for (i = 0; i < adaptations->children; ++i) {
	// 	zend_ast *adaptation_ast = adaptations->child[i];
	// 	switch (adaptation_ast->kind) {
	// 		case ZEND_AST_TRAIT_PRECEDENCE:
	// 			// zend_compile_trait_precedence(adaptation_ast);
	// 			break;
	// 		case ZEND_AST_TRAIT_ALIAS:
	// 			// zend_compile_trait_alias(adaptation_ast);
	// 			break;
	// 		EMPTY_SWITCH_DEFAULT_CASE()
	// 	}
	// }
}

void php2java_compile_class_decl(zend_ast *ast) /* {{{ */
{
	zend_ast_decl *decl = (zend_ast_decl *) ast;
	zend_ast *extends_ast = decl->child[0];
	zend_ast *implements_ast = decl->child[1];
	zend_ast *stmt_ast = decl->child[2];
	zend_string *name, *lcname;
	zend_class_entry *ce = zend_arena_alloc(&CG(arena), sizeof(zend_class_entry));
	zend_op *opline;
	znode declare_node, extends_node;

	zend_class_entry *original_ce = CG(active_class_entry);
	znode original_implementing_class = FC(implementing_class);

	if (EXPECTED((decl->flags & ZEND_ACC_ANON_CLASS) == 0)) {
		zend_string *unqualified_name = decl->name;

		if (CG(active_class_entry)) {
			zend_error_noreturn(E_COMPILE_ERROR, "Class declarations may not be nested");
		}

		zend_assert_valid_class_name(unqualified_name);
        // 使用命名空间链接当前类名，如果存在的话
		// name = zend_prefix_with_ns(unqualified_name);
		name = zend_new_interned_string(name);
		lcname = zend_string_tolower(name);

		if (FC(imports)) {
			zend_string *import_name = php2java_hash_find_ptr_lc(
				FC(imports), ZSTR_VAL(unqualified_name), ZSTR_LEN(unqualified_name));
			if (import_name && !zend_string_equals_ci(lcname, import_name)) {
				zend_error_noreturn(E_COMPILE_ERROR, "Cannot declare class %s "
						"because the name is already in use", ZSTR_VAL(name));
			}
		}

		php2java_register_seen_symbol(lcname, ZEND_SYMBOL_CLASS);
	} else {
		name = php2java_generate_anon_class_name(decl->lex_pos);
		lcname = zend_string_tolower(name);
	}
	lcname = zend_new_interned_string(lcname);

	ce->type = ZEND_USER_CLASS;
	ce->name = name;
	zend_initialize_class_data(ce, 1);

	ce->ce_flags |= decl->flags;
	ce->info.user.filename = zend_get_compiled_filename();
	ce->info.user.line_start = decl->start_lineno;
	ce->info.user.line_end = decl->end_lineno;

	if (decl->doc_comment) {
		ce->info.user.doc_comment = zend_string_copy(decl->doc_comment);
	}

	if (UNEXPECTED((decl->flags & ZEND_ACC_ANON_CLASS))) {
		/* Serialization is not supported for anonymous classes */
		ce->serialize = zend_class_serialize_deny;
		ce->unserialize = zend_class_unserialize_deny;
	}

	if (extends_ast) {
		if (!php2java_is_const_default_class_ref(extends_ast)) {
			zend_string *extends_name = zend_ast_get_str(extends_ast);
			zend_error_noreturn(E_COMPILE_ERROR,
				"Cannot use '%s' as class name as it is reserved", ZSTR_VAL(extends_name));
		}

		php2java_compile_class_ref(&extends_node, extends_ast, 0);
		ce->ce_flags |= ZEND_ACC_INHERITED;
	}

	opline = get_next_op(CG(active_op_array));
	// zend_make_var_result(&declare_node, opline);

	// GET_NODE(&FC(implementing_class), opline->result);

	opline->op1_type = IS_CONST;
	// LITERAL_STR(opline->op1, lcname);

	if (decl->flags & ZEND_ACC_ANON_CLASS) {
		if (extends_ast) {
			opline->opcode = ZEND_DECLARE_ANON_INHERITED_CLASS;
			// SET_NODE(opline->op2, &extends_node);
		} else {
			opline->opcode = ZEND_DECLARE_ANON_CLASS;
		}

		if (!zend_hash_exists(CG(class_table), lcname)) {
			zend_hash_add_ptr(CG(class_table), lcname, ce);
		} else {
			/* This anonymous class has been included, reuse the existing definition.
			 * NB: This behavior is buggy, and this should always result in a separate
			 * class declaration. However, until the problem of RTD key collisions is
			 * solved, this gives a behavior close to what is expected. */
			zval zv;
			ZVAL_PTR(&zv, ce);
			destroy_zend_class(&zv);
			ce = zend_hash_find_ptr(CG(class_table), lcname);

			/* Manually replicate emission of necessary inheritance opcodes here. We cannot
			 * reuse the general code, as we only want to emit the opcodes, without modifying
			 * the reused class definition. */
			if (ce->ce_flags & ZEND_ACC_IMPLEMENT_TRAITS) {
				php2java_emit_op(NULL, ZEND_BIND_TRAITS, &declare_node, NULL);
			}
			if (implements_ast) {
				zend_ast_list *iface_list = zend_ast_get_list(implements_ast);
				uint32_t i;
				for (i = 0; i < iface_list->children; i++) {
					opline = php2java_emit_op(NULL, ZEND_ADD_INTERFACE, &declare_node, NULL);
					opline->op2_type = IS_CONST;
					// opline->op2.constant = zend_add_class_name_literal(CG(active_op_array),
					// 	zend_resolve_class_name_ast(iface_list->child[i]));
				}
				php2java_emit_op(NULL, ZEND_VERIFY_ABSTRACT_CLASS, &declare_node, NULL);
			}
			return;
		}
	} else {
		zend_string *key;

		if (extends_ast) {
			opline->opcode = ZEND_DECLARE_INHERITED_CLASS;
			// SET_NODE(opline->op2, &extends_node);
		} else {
			opline->opcode = ZEND_DECLARE_CLASS;
			// SET_UNUSED(opline->op2);
		}

		// key = zend_build_runtime_definition_key(lcname, decl->lex_pos);
		/* RTD key is placed after lcname literal in op1 */
		// zend_add_literal_string(CG(active_op_array), &key);

		zend_hash_update_ptr(CG(class_table), key, ce);
	}

	CG(active_class_entry) = ce;

	php2java_compile_stmt(stmt_ast);

	/* Reset lineno for final opcodes and errors */
	CG(zend_lineno) = ast->lineno;

	if (ce->num_traits == 0) {
		/* For traits this check is delayed until after trait binding */
		zend_check_deprecated_constructor(ce);
	}

	if (ce->constructor) {
		ce->constructor->common.fn_flags |= ZEND_ACC_CTOR;
		if (ce->constructor->common.fn_flags & ZEND_ACC_STATIC) {
			zend_error_noreturn(E_COMPILE_ERROR, "Constructor %s::%s() cannot be static",
				ZSTR_VAL(ce->name), ZSTR_VAL(ce->constructor->common.function_name));
		}
		if (ce->constructor->common.fn_flags & ZEND_ACC_HAS_RETURN_TYPE) {
			zend_error_noreturn(E_COMPILE_ERROR,
				"Constructor %s::%s() cannot declare a return type",
				ZSTR_VAL(ce->name), ZSTR_VAL(ce->constructor->common.function_name));
		}
	}
	if (ce->destructor) {
		ce->destructor->common.fn_flags |= ZEND_ACC_DTOR;
		if (ce->destructor->common.fn_flags & ZEND_ACC_STATIC) {
			zend_error_noreturn(E_COMPILE_ERROR, "Destructor %s::%s() cannot be static",
				ZSTR_VAL(ce->name), ZSTR_VAL(ce->destructor->common.function_name));
		} else if (ce->destructor->common.fn_flags & ZEND_ACC_HAS_RETURN_TYPE) {
			zend_error_noreturn(E_COMPILE_ERROR,
				"Destructor %s::%s() cannot declare a return type",
				ZSTR_VAL(ce->name), ZSTR_VAL(ce->destructor->common.function_name));
		}
	}
	if (ce->clone) {
		if (ce->clone->common.fn_flags & ZEND_ACC_STATIC) {
			zend_error_noreturn(E_COMPILE_ERROR, "Clone method %s::%s() cannot be static",
				ZSTR_VAL(ce->name), ZSTR_VAL(ce->clone->common.function_name));
		} else if (ce->clone->common.fn_flags & ZEND_ACC_HAS_RETURN_TYPE) {
			zend_error_noreturn(E_COMPILE_ERROR,
				"%s::%s() cannot declare a return type",
				ZSTR_VAL(ce->name), ZSTR_VAL(ce->clone->common.function_name));
		}
	}

	/* Check for traits and proceed like with interfaces.
	 * The only difference will be a combined handling of them in the end.
	 * Thus, we need another opcode here. */
	if (ce->num_traits > 0) {
		ce->traits = NULL;
		ce->num_traits = 0;
		ce->ce_flags |= ZEND_ACC_IMPLEMENT_TRAITS;

		php2java_emit_op(NULL, ZEND_BIND_TRAITS, &declare_node, NULL);
	}

	if (implements_ast) {
		// zend_compile_implements(&declare_node, implements_ast);
	}

	if (!(ce->ce_flags & (ZEND_ACC_INTERFACE|ZEND_ACC_EXPLICIT_ABSTRACT_CLASS))
		&& (extends_ast || implements_ast)
	) {
		zend_verify_abstract_class(ce);
		if (implements_ast) {
			php2java_emit_op(NULL, ZEND_VERIFY_ABSTRACT_CLASS, &declare_node, NULL);
		}
	}

	/* Inherit interfaces; reset number to zero, we need it for above check and
	 * will restore it during actual implementation.
	 * The ZEND_ACC_IMPLEMENT_INTERFACES flag disables double call to
	 * zend_verify_abstract_class() */
	if (ce->num_interfaces > 0) {
		ce->interfaces = NULL;
		ce->num_interfaces = 0;
		ce->ce_flags |= ZEND_ACC_IMPLEMENT_INTERFACES;
	}

	FC(implementing_class) = original_implementing_class;
	CG(active_class_entry) = original_ce;
}

void php2java_compile_group_use(zend_ast *ast) /* {{{ */
{
	// uint32_t i;
	// zend_string *ns = zend_ast_get_str(ast->child[0]);
	// zend_ast_list *list = zend_ast_get_list(ast->child[1]);

	// for (i = 0; i < list->children; i++) {
	// 	zend_ast *inline_use, *use = list->child[i];
	// 	zval *name_zval = zend_ast_get_zval(use->child[0]);
	// 	zend_string *name = Z_STR_P(name_zval);
	// 	zend_string *compound_ns = zend_concat_names(ZSTR_VAL(ns), ZSTR_LEN(ns), ZSTR_VAL(name), ZSTR_LEN(name));
	// 	zend_string_release(name);
	// 	ZVAL_STR(name_zval, compound_ns);
	// 	inline_use = zend_ast_create_list(1, ZEND_AST_USE, use);
	// 	inline_use->attr = ast->attr ? ast->attr : use->attr;
	// 	zend_compile_use(inline_use);
	// }
}

void php2java_compile_use(zend_ast *ast) /* {{{ */
{
	// zend_ast_list *list = zend_ast_get_list(ast);
	// uint32_t i;
	// zend_string *current_ns = FC(current_namespace);
	// uint32_t type = ast->attr;
	// HashTable *current_import = zend_get_import_ht(type);
	// zend_bool case_sensitive = type == ZEND_SYMBOL_CONST;

	// for (i = 0; i < list->children; ++i) {
	// 	zend_ast *use_ast = list->child[i];
	// 	zend_ast *old_name_ast = use_ast->child[0];
	// 	zend_ast *new_name_ast = use_ast->child[1];
	// 	zend_string *old_name = zend_ast_get_str(old_name_ast);
	// 	zend_string *new_name, *lookup_name;

	// 	if (new_name_ast) {
	// 		new_name = zend_string_copy(zend_ast_get_str(new_name_ast));
	// 	} else {
	// 		const char *unqualified_name;
	// 		size_t unqualified_name_len;
	// 		if (zend_get_unqualified_name(old_name, &unqualified_name, &unqualified_name_len)) {
	// 			/* The form "use A\B" is equivalent to "use A\B as B" */
	// 			new_name = zend_string_init(unqualified_name, unqualified_name_len, 0);
	// 		} else {
	// 			new_name = zend_string_copy(old_name);

	// 			if (!current_ns) {
	// 				if (type == T_CLASS && zend_string_equals_literal(new_name, "strict")) {
	// 					zend_error_noreturn(E_COMPILE_ERROR,
	// 						"You seem to be trying to use a different language...");
	// 				}

	// 				zend_error(E_WARNING, "The use statement with non-compound name '%s' "
	// 					"has no effect", ZSTR_VAL(new_name));
	// 			}
	// 		}
	// 	}

	// 	if (case_sensitive) {
	// 		lookup_name = zend_string_copy(new_name);
	// 	} else {
	// 		lookup_name = zend_string_tolower(new_name);
	// 	}

	// 	if (type == ZEND_SYMBOL_CLASS && zend_is_reserved_class_name(new_name)) {
	// 		zend_error_noreturn(E_COMPILE_ERROR, "Cannot use %s as %s because '%s' "
	// 			"is a special class name", ZSTR_VAL(old_name), ZSTR_VAL(new_name), ZSTR_VAL(new_name));
	// 	}

	// 	if (current_ns) {
	// 		zend_string *ns_name = zend_string_alloc(ZSTR_LEN(current_ns) + 1 + ZSTR_LEN(new_name), 0);
	// 		zend_str_tolower_copy(ZSTR_VAL(ns_name), ZSTR_VAL(current_ns), ZSTR_LEN(current_ns));
	// 		ZSTR_VAL(ns_name)[ZSTR_LEN(current_ns)] = '\\';
	// 		memcpy(ZSTR_VAL(ns_name) + ZSTR_LEN(current_ns) + 1, ZSTR_VAL(lookup_name), ZSTR_LEN(lookup_name) + 1);

	// 		if (zend_have_seen_symbol(ns_name, type)) {
	// 			zend_check_already_in_use(type, old_name, new_name, ns_name);
	// 		}

	// 		zend_string_free(ns_name);
	// 	} else {
	// 		if (zend_have_seen_symbol(lookup_name, type)) {
	// 			zend_check_already_in_use(type, old_name, new_name, lookup_name);
	// 		}
	// 	}

	// 	zend_string_addref(old_name);
	// 	old_name = zend_new_interned_string(old_name);
	// 	if (!zend_hash_add_ptr(current_import, lookup_name, old_name)) {
	// 		zend_error_noreturn(E_COMPILE_ERROR, "Cannot use%s %s as %s because the name "
	// 			"is already in use", zend_get_use_type_str(type), ZSTR_VAL(old_name), ZSTR_VAL(new_name));
	// 	}

	// 	zend_string_release(lookup_name);
	// 	zend_string_release(new_name);
	// }
}

void php2java_compile_const_decl(zend_ast *ast) /* {{{ */
{
	zend_ast_list *list = zend_ast_get_list(ast);
	uint32_t i;
	for (i = 0; i < list->children; ++i) {
		zend_ast *const_ast = list->child[i];
		zend_ast *name_ast = const_ast->child[0];
		zend_ast *value_ast = const_ast->child[1];
		zend_string *unqualified_name = zend_ast_get_str(name_ast);

		zend_string *name;
		znode name_node, value_node;
		zval *value_zv = &value_node.u.constant;

		value_node.op_type = IS_CONST;
		zend_const_expr_to_zval(value_zv, value_ast);

		// if (zend_lookup_reserved_const(ZSTR_VAL(unqualified_name), ZSTR_LEN(unqualified_name))) {
		// 	zend_error_noreturn(E_COMPILE_ERROR,
		// 		"Cannot redeclare constant '%s'", ZSTR_VAL(unqualified_name));
		// }

		name = zend_prefix_with_ns(unqualified_name);
		name = zend_new_interned_string(name);

		if (FC(imports_const)) {
			zend_string *import_name = zend_hash_find_ptr(FC(imports_const), unqualified_name);
			if (import_name && !zend_string_equals(import_name, name)) {
				zend_error_noreturn(E_COMPILE_ERROR, "Cannot declare const %s because "
					"the name is already in use", ZSTR_VAL(name));
			}
		}

		name_node.op_type = IS_CONST;
		ZVAL_STR(&name_node.u.constant, name);

		php2java_emit_op(NULL, ZEND_DECLARE_CONST, &name_node, &value_node);

		// zend_register_seen_symbol(name, ZEND_SYMBOL_CONST);
	}
}

void php2java_compile_namespace(zend_ast *ast) /* {{{ */
{
	// zend_ast *name_ast = ast->child[0];
	// zend_ast *stmt_ast = ast->child[1];
	// zend_string *name;
	// zend_bool with_bracket = stmt_ast != NULL;

	// /* handle mixed syntax declaration or nested namespaces */
	// if (!FC(has_bracketed_namespaces)) {
	// 	if (FC(current_namespace)) {
	// 		/* previous namespace declarations were unbracketed */
	// 		if (with_bracket) {
	// 			zend_error_noreturn(E_COMPILE_ERROR, "Cannot mix bracketed namespace declarations "
	// 				"with unbracketed namespace declarations");
	// 		}
	// 	}
	// } else {
	// 	/* previous namespace declarations were bracketed */
	// 	if (!with_bracket) {
	// 		zend_error_noreturn(E_COMPILE_ERROR, "Cannot mix bracketed namespace declarations "
	// 			"with unbracketed namespace declarations");
	// 	} else if (FC(current_namespace) || FC(in_namespace)) {
	// 		zend_error_noreturn(E_COMPILE_ERROR, "Namespace declarations cannot be nested");
	// 	}
	// }

	// if (((!with_bracket && !FC(current_namespace))
	// 	 || (with_bracket && !FC(has_bracketed_namespaces))) && CG(active_op_array)->last > 0
	// ) {
	// 	/* ignore ZEND_EXT_STMT and ZEND_TICKS */
	// 	uint32_t num = CG(active_op_array)->last;
	// 	while (num > 0 &&
	// 	       (CG(active_op_array)->opcodes[num-1].opcode == ZEND_EXT_STMT ||
	// 	        CG(active_op_array)->opcodes[num-1].opcode == ZEND_TICKS)) {
	// 		--num;
	// 	}
	// 	if (num > 0) {
	// 		zend_error_noreturn(E_COMPILE_ERROR, "Namespace declaration statement has to be "
	// 			"the very first statement or after any declare call in the script");
	// 	}
	// }

	// if (FC(current_namespace)) {
	// 	zend_string_release(FC(current_namespace));
	// }

	// if (name_ast) {
	// 	name = zend_ast_get_str(name_ast);

	// 	if (ZEND_FETCH_CLASS_DEFAULT != php2java_get_class_fetch_type(name)) {
	// 		zend_error_noreturn(E_COMPILE_ERROR, "Cannot use '%s' as namespace name", ZSTR_VAL(name));
	// 	}

	// 	FC(current_namespace) = zend_string_copy(name);
	// } else {
	// 	FC(current_namespace) = NULL;
	// }

	// zend_reset_import_tables();

	// FC(in_namespace) = 1;
	// if (with_bracket) {
	// 	FC(has_bracketed_namespaces) = 1;
	// }

	// if (stmt_ast) {
	// 	zend_compile_top_stmt(stmt_ast);
	// 	zend_end_namespace();
	// }
}

void php2java_compile_halt_compiler(zend_ast *ast) /* {{{ */
{
	// zend_ast *offset_ast = ast->child[0];
	// zend_long offset = Z_LVAL_P(zend_ast_get_zval(offset_ast));

	// zend_string *filename, *name;
	// const char const_name[] = "__COMPILER_HALT_OFFSET__";

	// if (FC(has_bracketed_namespaces) && FC(in_namespace)) {
	// 	zend_error_noreturn(E_COMPILE_ERROR,
	// 		"__HALT_COMPILER() can only be used from the outermost scope");
	// }

	// filename = zend_get_compiled_filename();
	// name = zend_mangle_property_name(const_name, sizeof(const_name) - 1,
	// 	ZSTR_VAL(filename), ZSTR_LEN(filename), 0);

	// zend_register_long_constant(ZSTR_VAL(name), ZSTR_LEN(name), offset, CONST_CS, 0);
	// zend_string_release(name);
}

void php2java_compile_stmt(zend_ast *ast) /* {{{ */
{
	if (!ast) {
		return;
	}

	CG(zend_lineno) = ast->lineno;

	switch (ast->kind) {
		case ZEND_AST_STMT_LIST:
			php2java_compile_stmt_list(ast);
			break;
		case ZEND_AST_GLOBAL:
			//zend_compile_global_var(ast);
			break;
		case ZEND_AST_STATIC:
			php2java_compile_static_var(ast);
			break;
		case ZEND_AST_UNSET:
			//zend_compile_unset(ast);
			break;
		case ZEND_AST_RETURN:
			php2java_compile_return(ast);
			break;
		case ZEND_AST_ECHO:
			php2java_compile_echo(ast);
			break;
		case ZEND_AST_THROW:
			php2java_compile_throw(ast);
			break;
		case ZEND_AST_BREAK:
		case ZEND_AST_CONTINUE:
			php2java_compile_break_continue(ast);
			break;
		case ZEND_AST_GOTO:
			php2java_compile_goto(ast);
			break;
		case ZEND_AST_LABEL:
			php2java_compile_label(ast);
			break;
		case ZEND_AST_WHILE:
			php2java_compile_while(ast);
			break;
		case ZEND_AST_DO_WHILE:
			php2java_compile_while(ast);
			break;
		case ZEND_AST_FOR:
			php2java_compile_for(ast);
			break;
		case ZEND_AST_FOREACH:
			php2java_compile_foreach(ast);
			break;
		case ZEND_AST_IF:
			php2java_compile_if(ast);
			break;
		case ZEND_AST_SWITCH:
			php2java_compile_switch(ast);
			break;
		case ZEND_AST_TRY:
			php2java_compile_try(ast);
			break;
		case ZEND_AST_DECLARE:
			php2java_compile_declare(ast);
			break;
		case ZEND_AST_FUNC_DECL:
		case ZEND_AST_METHOD:
			php2java_compile_func_decl(NULL, ast);
			break;
		case ZEND_AST_PROP_DECL:
			php2java_compile_prop_decl(ast);
			break;
		case ZEND_AST_CLASS_CONST_DECL:
			php2java_compile_class_const_decl(ast);
			break;
		case ZEND_AST_USE_TRAIT:
			php2java_compile_use_trait(ast);
			break;
		case ZEND_AST_CLASS:
			php2java_compile_class_decl(ast);
			break;
		case ZEND_AST_GROUP_USE:
			php2java_compile_group_use(ast);
			break;
		case ZEND_AST_USE:
			php2java_compile_use(ast);
			break;
		case ZEND_AST_CONST_DECL:
			php2java_compile_const_decl(ast);
			break;
		case ZEND_AST_NAMESPACE:
			php2java_compile_namespace(ast);
			break;
		case ZEND_AST_HALT_COMPILER:
			php2java_compile_halt_compiler(ast);
			break;
		default:
		{
			znode result;
			php2java_compile_expr(&result, ast);
			// zend_do_free(&result);
		}
	}

	// if (FC(declarables).ticks && !zend_is_unticked_stmt(ast)) {
	// 	zend_emit_tick();
	// }
}

void php2java_compile_top_stmt(zend_ast *ast) /* {{{ */
{
	if (!ast) {
		return;
	}

	if (ast->kind == ZEND_AST_STMT_LIST) {
		zend_ast_list *list = zend_ast_get_list(ast);
		uint32_t i;
		for (i = 0; i < list->children; ++i) {
			php2java_compile_top_stmt(list->child[i]);
		}
		return;
	}

	php2java_compile_stmt(ast);

	if (ast->kind != ZEND_AST_NAMESPACE && ast->kind != ZEND_AST_HALT_COMPILER) {
		// zend_verify_namespace();
	}
	if (ast->kind == ZEND_AST_FUNC_DECL || ast->kind == ZEND_AST_CLASS) {
		CG(zend_lineno) = ((zend_ast_decl *) ast)->end_lineno;
		// zend_do_early_binding();
	}
}

void php2java_compile_file(zend_file_handle *primary_file) {
	char *filename = to_java_filename(primary_file->filename);

	zend_lex_state original_lex_state;
	zend_save_lexical_state(&original_lex_state);

	if (open_file_for_scanning(primary_file)==FAILURE) {
		zend_message_dispatcher(ZMSG_FAILED_REQUIRE_FOPEN, primary_file->filename);
		zend_bailout();
	} else {

		zend_bool original_in_compilation = CG(in_compilation);

		CG(in_compilation) = 1;
		CG(ast) = NULL;
		CG(ast_arena) = zend_arena_create(1024 * 32);

		if (!zendparse()) {

			appendInt(poolbuf, JAVA_MAGIC_NUMBER);
			appendChar(poolbuf, 0);
			appendChar(poolbuf, JAVA_SE_8_MAJOR);
			
			int pool_count_idx = poolbuf->length;
			appendChar(poolbuf, 0);

			// ast结构遍历
			php2java_compile_top_stmt(CG(ast));

			// 常量池的个数写入
			appendCharByPos(poolbuf, poolbuf->length, pool_count_idx);
		}

		zend_ast_destroy(CG(ast));
		zend_arena_destroy(CG(ast_arena));
		CG(in_compilation) = original_in_compilation;

	}
	zend_restore_lexical_state(&original_lex_state);
	zend_destroy_file_handle(primary_file);

	efree(filename);
}


PHPAPI int transfer_by_gramar_tree(zend_file_handle *primary_file, int java_version)
{

    int ret_val = SUCCESS;
    zend_try
    {
        php2java_startup();
		php2java_compile_file(primary_file);	
        php2java_shutdown();
    }
    zend_end_try();

    if (EG(exception))
    {
        zend_try
        {
            zend_exception_error(EG(exception), E_ERROR);
        }
        zend_end_try();
    }

    return ret_val;
}
