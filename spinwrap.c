/* spinwrap - generate wrappers to call Spin from C++ code */

/* Copyright (c) 2014, David Betz */
/* based on code Copyright (c) 2008, Steve Denson */
/* based on code by Dave Hein */
/* MIT license */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <ctype.h>

// define OLD_SPIN_FILE_PARSER to use the simple Spin file parser

#ifdef CATALINA
#define SPINCOMPILER        "spinnaker"
#else
#define SPINCOMPILER        "openspin"
#endif

#define TOKEN_MAX           33

#ifndef LINE_MAX
#define LINE_MAX            1024
#endif

#define DEFAULT_STACK_SIZE  64

enum {
    F_INIT    = 0x00000001,
    F_UTF16BE = 0x00000002,
    F_UTF16LE = 0x00000004
};

typedef struct {
    FILE *fp;
    int flags;
    uint8_t line[LINE_MAX];
    uint8_t *linePtr;
    int lineNumber;
    char token[TOKEN_MAX];
    int value;
} ParseContext;

enum {
    T_UNKNOWN = 0,
    T_EOL = -1,
    T_IDENTIFIER = -2,
    T_NUMBER = -3
};

#define firstidchar(c)  (isalpha(c) || (c) == '_')
#define idchar(c)       (firstidchar(c) || isdigit(c))

enum {
	TYPE_INTEGER,
	TYPE_FLOAT
};

typedef struct Constant Constant;
struct Constant {
	int type;
	union {
		uint32_t integerValue;
		double floatValue;
	} v;
	Constant *next;
	char name[1];
};

typedef struct MethodArg MethodArg;
struct MethodArg {
	int index;
    MethodArg *next;
    char name[1];
};

typedef struct Method Method;
struct Method {
    Method *next;
    int index;
    MethodArg *arguments;
    MethodArg **pNextArgument;
    int argumentCount;
    char name[1];
};

typedef struct Object Object;
struct Object {
    Object *next;
    Constant *constants;
    Constant **pNextConstant;
    Method *methods;
    Method **pNextMethod;
    int methodCount;
    char name[1];
};

typedef struct {
    uint16_t code;
    uint16_t locals;
} SpinMethodPtr;

typedef struct {
    uint16_t object;
    uint16_t vars;
} SpinObjectPtr;

typedef struct {
    uint16_t next;
    uint8_t  methodcnt; /* this seems to be the method count + 1 */
    uint8_t  objectcnt;
} SpinObj;

typedef struct {
    uint32_t    clkfreq;
    uint8_t     clkmode;
    uint8_t     chksum;
    uint16_t    pbase;
    uint16_t    vbase;
    uint16_t    dbase;
    uint16_t    pcurr;
    uint16_t    dcurr;
} SpinHdr;

#define FIELDOFFSET(s, f)   ((int)&((s *)0)->f)

/* globals */
static char spinName[PATH_MAX];
static char rootPath[PATH_MAX] = "";
static char h_path[PATH_MAX] = "";
static char source_path[PATH_MAX] = "";
static char spin_path[PATH_MAX] = "";
static char symbol_path[PATH_MAX] = "";
static char binary_path[PATH_MAX] = "";
static char spin_args[LINE_MAX] = "";
static int generateC = 0;
static int debug = 0;

/* forward declarations */
static void Usage(void);
static void CompleteOutputPaths(char *name);
static Object *ProcessSpinSymbolFile(char *path, char *name);
#ifndef OLD_SPIN_FILE_PARSER
static Constant *AddConstant(Object *object, char *name, int type);
#endif
static Method *AddMethod(Object *object, char *name);
static MethodArg *AddArgument(Method *method, char *name);
static void WriteCHeader(char *name, Object *objects, uint8_t *binary);
static void WriteCppHeader(char *name, Object *objects, uint8_t *binary, int stackSize);
static void WriteCStubs(char *name, Object *objects, int stackSize, uint8_t *binary, int binarySize);
static void WriteCppStubs(char *name, Object *objects, uint8_t *binary, int binarySize);
static void WriteProxy(char *name, Object *objects);
static void *safe_calloc(size_t size);
static void rootname(char *path, char *name);
static int getLine(ParseContext *c);
static int token(ParseContext *c);
static int SkipSpaces(ParseContext *c);
static uint8_t *ReadEntireFile(char *name, long *pSize);
static void DumpSpinBinary(uint8_t *binary);

int main(int argc, char *argv[])
{
    Object *object = NULL;
    char *spinFile = NULL;
    char cmd[1024], *arg;
    uint8_t *binary;
    long binarySize;
    int stackSize = DEFAULT_STACK_SIZE;
    int sts, i;
    
    /* get the arguments */
    for(i = 1; i < argc; ++i) {

        /* handle switches */
        if(argv[i][0] == '-') {
            switch(argv[i][1]) {
            case 'c':   // generate c instead of c++
                generateC = 1;
                break;
            case 'd':   // debug
                debug = 1;
                break;
            case 'p':   // output path
                if (argv[i][2])
                    strcpy(rootPath, &argv[i][2]);
                else if (++i < argc)
                    strcpy(rootPath, argv[i]);
                else
                    Usage();
                break;
            case 's':   // stack size
                if (argv[i][2])
                    stackSize = atoi(&argv[i][2]);
                else if (++i < argc)
                    stackSize = atoi(argv[i]);
                else
                    Usage();
                break;
            case 'S':   // option to pass to openspin
                if (argv[i][2] != ',')
                    Usage();
                else if (argv[i][3])
                    arg = &argv[i][3];
                else if (++i < argc)
                    arg = argv[i];
                else
                    Usage();
                strcat(spin_args, " ");
                strcat(spin_args, arg);
                break;
            default:
                Usage();
                break;
            }
        }

        /* process a spin file */
        else {
            if (spinFile)
                Usage();
            spinFile = argv[i];
        }
    }
    
    if (!spinFile)
        Usage();
        
    rootname(spinFile, spinName);
    
    /* determine the paths to each of the output files */
    CompleteOutputPaths(spinName);
    
	/* get the spin interface */
	//object = ProcessSpinFile(spinFile, spinName);
	object = ProcessSpinSymbolFile(spinFile, spinName);
	
    /* create the spin proxy */
    WriteProxy(spin_path, object);
    
    /* compile the spin proxy */
    sprintf(cmd, "%s%s -o \"%s\" \"%s\"", SPINCOMPILER, spin_args, binary_path, spin_path);
    if (debug)
        printf("cmd: %s\n", cmd);
    if ((sts = system(cmd)) != 0) {
        fprintf(stderr, "error: openspin compile failed, error code %d\n", sts);
        return 1;
    }
    
    /* read the generated binary */
    if (!(binary = ReadEntireFile(binary_path, &binarySize))) {
        fprintf(stderr, "error: can't read %s\n", binary_path);
        return 1;
    }

    if (debug)
        DumpSpinBinary(binary);

    /* create the header and stubs files */
    if (generateC) {
        WriteCHeader(h_path, object, binary);
        WriteCStubs(source_path, object, stackSize, binary, (int)binarySize);
    }
    else {
        WriteCppHeader(h_path, object, binary, stackSize);
        WriteCppStubs(source_path, object, binary, (int)binarySize);
    }
    
    /* return successfully */
    return 0;
}

/* Usage - display a usage message and exit */
static void Usage(void)
{
    printf("\
usage: spinwrap [ -c ]\n\
                [ -d ]\n\
                [ -p <output-path-root> ]\n\
                [ -s <stack-size> ]\n\
                [ -S,<openspin option> ]... <spin-file>...\n");
    exit(1);
}

static void CompleteOutputPaths(char *name)
{
    if (rootPath[0] != '\0' && rootPath[strlen(rootPath) - 1] != '/')
        strcat(rootPath, "/");
    sprintf(symbol_path, "%s%s.sym", rootPath, name);
    sprintf(h_path, "%s%s.h", rootPath, name);
    if (generateC)
        sprintf(source_path, "%s%s.c", rootPath, name);
    else
        sprintf(source_path, "%s%s.cpp", rootPath, name);
    sprintf(spin_path, "%s%s_proxy.spin", rootPath, name);
    sprintf(binary_path, "%s%s.binary", rootPath, name);
}

#ifdef OLD_SPIN_FILE_PARSER

static Object *ProcessSpinSymbolFile(char *path, char *name)
{
    ParseContext *c = safe_calloc(sizeof(ParseContext));
    Object *object = (Object *)safe_calloc(sizeof(Object) + strlen(name));

    strcpy(object->name, name);
    object->pNextConstant = &object->constants;
    object->pNextMethod = &object->methods;
    
    if (!(c->fp = fopen(path, "rb"))) {
        fprintf(stderr, "error: can't open %s\n", name);
        exit(1);
    }
    c->lineNumber = 0;
    
    while (getLine(c)) {
        int tkn;
        if ((tkn = token(c)) == T_IDENTIFIER && strcasecmp(c->token, "PUB") == 0) {
            if ((tkn = token(c)) == T_IDENTIFIER) {
                Method *method = AddMethod(object, c->token);
                if ((tkn = token(c)) == '(') {
                    do {
                        if ((tkn = token(c)) != T_IDENTIFIER) {
                            fprintf(stderr, "error: method syntax error -- expecting an identifier on line %d\n", c->lineNumber);
                            exit(1);
                        }
                        AddArgument(method, c->token);
                    } while ((tkn = token(c)) == ',');
                    if (tkn != ')') {
                        fprintf(stderr, "error: method syntax error -- expecting a close paren on line %d\n", c->lineNumber);
                        exit(1);
                    }
                }
            }
        }
    }
    
    fclose(c->fp);
    free(c);
    return object;
}

#else

/*

OpenSpin -s symbol output format:

CON, _clkmode, 1032
CON, _xinfreq, 5000000
CONF, testFloat, 6.280000
PARAM, Start, PinA, 0, 0
PARAM, Start, PinB, 0, 1
PUB, Start, 0, 2
PUB, End, 1, 0

The params for a pub appear just before the pub.

CON line is: CON name, integer value
CONF line is: CON name, float value
PARAM line is this: PUB name, Param Name, PUB Index, Param Index
PUB line is this: PUB name, PUB index, Param count

*/

static Object *ProcessSpinSymbolFile(char *path, char *name)
{
    ParseContext *c = safe_calloc(sizeof(ParseContext));
    Object *object = (Object *)safe_calloc(sizeof(Object) + strlen(name));
    char cmd[1024];
    int sts;
    
    c->flags |= F_INIT;
    
    strcpy(object->name, name);
    object->pNextConstant = &object->constants;
    object->pNextMethod = &object->methods;
    
    /* generate symbols to use to build the proxy and wrappers */
    sprintf(cmd, "%s%s -s \"%s\" > \"%s\"", SPINCOMPILER, spin_args, path, symbol_path);
    if (debug)
        printf("cmd: %s\n", cmd);
    if ((sts = system(cmd)) != 0) {
        fprintf(stderr, "error: openspin symbol table generation failed, error code %d\n", sts);
        exit(1);
    }
    
    if (!(c->fp = fopen(symbol_path, "r"))) {
        fprintf(stderr, "error: can't open %s\n", symbol_path);
        exit(1);
    }
    c->lineNumber = 0;
    
    while (getLine(c)) {
        int tkn;
        if ((tkn = token(c)) == T_IDENTIFIER) {
        	if (strcasecmp(c->token, "CON") == 0) {
				char name[TOKEN_MAX];
				Constant *constant;
				
				if ((tkn = token(c)) != ',') {
					fprintf(stderr, "warning: syntax error on line %d\n", c->lineNumber);
					continue;
				}
				if ((tkn = token(c)) != T_IDENTIFIER) {
					fprintf(stderr, "warning: syntax error on line %d\n", c->lineNumber);
					continue;
				}
				strcpy(name, c->token);
				if ((tkn = token(c)) != ',') {
					fprintf(stderr, "warning: syntax error on line %d\n", c->lineNumber);
					continue;
				}
				if ((tkn = token(c)) != T_NUMBER) {
					fprintf(stderr, "warning: syntax error on line %d\n", c->lineNumber);
					continue;
				}
				
				constant = AddConstant(object, name, TYPE_INTEGER);
				constant->v.integerValue = c->value;
				
				if (debug)
					printf("CON: name '%s', value %u\n", name, constant->v.integerValue);
        	}
        	else if (strcasecmp(c->token, "CONF") == 0) {
				Constant *constant;
				
				if ((tkn = token(c)) != ',') {
					fprintf(stderr, "warning: syntax error on line %d\n", c->lineNumber);
					continue;
				}
				if ((tkn = token(c)) != T_IDENTIFIER) {
					fprintf(stderr, "warning: syntax error on line %d\n", c->lineNumber);
					continue;
				}
				strcpy(name, c->token);
				if ((tkn = token(c)) != ',') {
					fprintf(stderr, "warning: syntax error on line %d\n", c->lineNumber);
					continue;
				}

				constant = AddConstant(object, name, TYPE_FLOAT);
				constant->v.floatValue = strtod((char *)c->linePtr, NULL);
				
				if (debug)
					printf("CONF: name '%s', value %g\n", name, constant->v.floatValue);
        	}
        	else if (strcasecmp(c->token, "PARAM") == 0) {
        		char methodName[TOKEN_MAX];
        		char argumentName[TOKEN_MAX];
        		int methodIndex;
        		int argumentIndex;
        		Method *method;
        		MethodArg *argument;
        		
				if ((tkn = token(c)) != ',') {
					fprintf(stderr, "warning: syntax error on line %d\n", c->lineNumber);
					continue;
				}
				if ((tkn = token(c)) != T_IDENTIFIER) {
					fprintf(stderr, "warning: syntax error on line %d\n", c->lineNumber);
					continue;
				}
				strcpy(methodName, c->token);
				if ((tkn = token(c)) != ',') {
					fprintf(stderr, "warning: syntax error on line %d\n", c->lineNumber);
					continue;
				}
				if ((tkn = token(c)) != T_IDENTIFIER) {
					fprintf(stderr, "warning: syntax error on line %d\n", c->lineNumber);
					continue;
				}
				strcpy(argumentName, c->token);
				if ((tkn = token(c)) != ',') {
					fprintf(stderr, "warning: syntax error on line %d\n", c->lineNumber);
					continue;
				}
				if ((tkn = token(c)) != T_NUMBER) {
					fprintf(stderr, "warning: syntax error on line %d\n", c->lineNumber);
					continue;
				}
				methodIndex = c->value;
				if ((tkn = token(c)) != ',') {
					fprintf(stderr, "warning: syntax error on line %d\n", c->lineNumber);
					continue;
				}
				if ((tkn = token(c)) != T_NUMBER) {
					fprintf(stderr, "warning: syntax error on line %d\n", c->lineNumber);
					continue;
				}
				argumentIndex = c->value;
				
				if (debug)
					printf("PARAM: method '%s', parameter '%s', methodIndex %d, argumentIndex %d\n", methodName, argumentName, methodIndex, argumentIndex);
				
				method = AddMethod(object, methodName);
				argument = AddArgument(method, argumentName);

				if (argumentIndex != argument->index) {
					fprintf(stderr, "error: parameter index mismatch in symbol file\n");
					exit(1);
				}
        	}
        	else if (strcasecmp(c->token, "PUB") == 0) {
        		char methodName[TOKEN_MAX];
        		int methodIndex;
        		int argumentCount;
        		Method *method;
        		
				if ((tkn = token(c)) != ',') {
					fprintf(stderr, "warning: syntax error on line %d\n", c->lineNumber);
					continue;
				}
				if ((tkn = token(c)) != T_IDENTIFIER) {
					fprintf(stderr, "warning: syntax error on line %d\n", c->lineNumber);
					continue;
				}
				strcpy(methodName, c->token);
				if ((tkn = token(c)) != ',') {
					fprintf(stderr, "warning: syntax error on line %d\n", c->lineNumber);
					continue;
				}
				if ((tkn = token(c)) != T_NUMBER) {
					fprintf(stderr, "warning: syntax error on line %d\n", c->lineNumber);
					continue;
				}
				methodIndex = c->value;
				if ((tkn = token(c)) != ',') {
					fprintf(stderr, "warning: syntax error on line %d\n", c->lineNumber);
					continue;
				}
				if ((tkn = token(c)) != T_NUMBER) {
					fprintf(stderr, "warning: syntax error on line %d\n", c->lineNumber);
					continue;
				}
				argumentCount = c->value;
				
				if (debug)
					printf("PUB: method '%s', methodIndex %d, argumentCount %d\n", methodName, methodIndex, argumentCount);
				
				method = AddMethod(object, methodName);
				
				if (argumentCount != method->argumentCount) {
					fprintf(stderr, "error: parameter count mismatch in symbol file\n");
					exit(1);
				}
        	}
        }
    }
    
    fclose(c->fp);
    free(c);
    return object;
}

static Constant *AddConstant(Object *object, char *name, int type)
{
	Constant *constant;
	constant = (Constant *)safe_calloc(sizeof(Constant) + strlen(name));
	strcpy(constant->name, name);
	constant->type = type;
	*object->pNextConstant = constant;
	object->pNextConstant = &constant->next;
	return constant;
}

#endif

static Method *AddMethod(Object *object, char *name)
{
	Method *method;
	for (method = object->methods; method != NULL; method = method->next)
		if (strcasecmp(name, method->name) == 0)
			return method;
	method = (Method *)safe_calloc(sizeof(Method) + strlen(name));
	strcpy(method->name, name);
	method->index = object->methodCount++;
	method->pNextArgument = &method->arguments;
	*object->pNextMethod = method;
	object->pNextMethod = &method->next;
	return method;
}

static MethodArg *AddArgument(Method *method, char *name)
{
	MethodArg *argument = (MethodArg *)safe_calloc(sizeof(MethodArg) + strlen(name));
	strcpy(argument->name, name);
	argument->index = method->argumentCount++;
	*method->pNextArgument = argument;
	method->pNextArgument = &argument->next;
	return argument;
}

static void WriteCHeader(char *file, Object *object, uint8_t *binary)
{
    FILE *fp;
    if ((fp = fopen(file, "w")) != NULL) {
        SpinHdr *hdr = (SpinHdr *)binary;
        Method *method = object->methods;
        Constant *constant = object->constants;
        int varSize;
        
        fprintf(fp, "/* This file was generated by spinwrap. Do not edit! */\n");

        fprintf(fp, "\n#include <stdint.h>\n");
        
        /* can't have an array of size zero */
        varSize = hdr->dbase - hdr->vbase - 8;
        if (varSize <= 0)
            varSize = sizeof(uint32_t);
            
        if (constant) {
        	fputc('\n', fp);
        	while (constant != NULL) {
				switch (constant->type) {
				case TYPE_INTEGER:
					fprintf(fp, "#define %s_%s %u\n", object->name, constant->name, constant->v.integerValue);
					break;
				case TYPE_FLOAT:
					fprintf(fp, "#define %s_%s %g\n", object->name, constant->name, constant->v.floatValue);
					break;
				default:
					fprintf(fp, "error: unknown constant type %d\n", constant->type);
					exit(1);
				}
				constant = constant->next;
			}
		}

        fprintf(fp, "\ntypedef struct {\n");
        fprintf(fp, "  uint8_t variables[%d];\n", varSize);
        fprintf(fp, "} %s;\n", object->name);
        
        fprintf(fp, "\nvoid init_%s(%s *obj);\n", object->name, object->name);
        fprintf(fp, "void term_%s(%s *obj);\n", object->name, object->name);
        while (method != NULL) {
            MethodArg *argument = method->arguments;
            fprintf(fp, "uint32_t %s_%s(%s *obj", object->name, method->name, object->name);
            while (argument != NULL) {
                fprintf(fp, ", uint32_t %s", argument->name);
                argument = argument->next;
            }
            fprintf(fp, ");\n");
            method = method->next;
        }

        fclose(fp);
    }
}

static void WriteCppHeader(char *file, Object *object, uint8_t *binary, int stackSize)
{
    FILE *fp;
    if ((fp = fopen(file, "w")) != NULL) {
        SpinHdr *hdr = (SpinHdr *)binary;
        Method *method = object->methods;
        Constant *constant = object->constants;
        int varSize;
        
        fprintf(fp, "/* This file was generated by spinwrap. Do not edit! */\n");

        fprintf(fp, "\n#include <stdint.h>\n");
        
        /* can't have an array of size zero */
        varSize = hdr->dbase - hdr->vbase - 8;
        if (varSize <= 0)
            varSize = sizeof(uint32_t);
            
        fprintf(fp, "\nclass %s {\n", object->name);
        fprintf(fp, "public:\n");
        fprintf(fp, "  %s();\n", object->name);
        fprintf(fp, "  ~%s();\n", object->name);
        while (method != NULL) {
            MethodArg *argument = method->arguments;
            fprintf(fp, "  uint32_t %s(", method->name);
            while (argument != NULL) {
                fprintf(fp, "uint32_t %s", argument->name);
                if (argument->next != NULL)
                    fprintf(fp, ", ");
                argument = argument->next;
            }
            fprintf(fp, ");\n");
            method = method->next;
        }
        while (constant != NULL) {
        	switch (constant->type) {
        	case TYPE_INTEGER:
        		fprintf(fp, "  static const uint32_t %s = %u;\n", constant->name, constant->v.integerValue);
        		break;
        	case TYPE_FLOAT:
        		fprintf(fp, "  static const double %s = %g;\n", constant->name, constant->v.floatValue);
        		break;
        	default:
        		fprintf(fp, "error: unknown constant type %d\n", constant->type);
        		exit(1);
        	}
        	constant = constant->next;
        }
        fprintf(fp, "private:\n");
        fprintf(fp, "  uint8_t m_variables[%d];\n", varSize);
        fprintf(fp, "  static uint8_t *m_rootVars;\n");
        fprintf(fp, "  static uint16_t *m_pVarOffset;\n");
        fprintf(fp, "  static uint32_t m_stack[%d];\n", stackSize);
        fprintf(fp, "  static volatile uint32_t *volatile m_mailbox;\n");
        fprintf(fp, "  static int m_refCnt;\n");
        fprintf(fp, "  static int m_cogid;\n");
        fprintf(fp, "};\n");
        
        fclose(fp);
    }
}

static void WriteCStubs(char *file, Object *object, int stackSize, uint8_t *binary, int binarySize)
{
    FILE *fp;
    if ((fp = fopen(file, "w")) != NULL) {
        Method *method = object->methods;
        SpinHdr *hdr;
        SpinObj *obj;
        uint16_t dat, off;
        int cnt, i;
        
        fprintf(fp, "/* This file was generated by spinwrap. Do not edit! */\n");

        fprintf(fp, "\n#include <string.h>\n");
        fprintf(fp, "#include \"%s.h\"\n", object->name);
        
        fprintf(fp, "\n#ifdef __PROPELLER__\n");
        fprintf(fp, "#include <propeller.h>\n");
        fprintf(fp, "#endif\n");
        
        fprintf(fp, "\n#ifdef __CATALINA__\n");
        fprintf(fp, "#include <catalina_cog.h>\n");
        fprintf(fp, "#define cognew(code, params)    _coginit((int)(params) >> 2, (code) >> 2, ANY_COG)\n");
        fprintf(fp, "#define cogstop(id)             _cogstop(id)\n");
        fprintf(fp, "#endif\n");
        
        fprintf(fp, "\n#define SPINVM 0xf004\n");
        
        fprintf(fp, "\nstatic uint8_t *%s_rootVars;\n", object->name);
        fprintf(fp, "static uint16_t *%s_pVarOffset;\n", object->name);
        fprintf(fp, "static uint32_t %s_stack[%d];\n", object->name, stackSize);
        fprintf(fp, "static volatile uint32_t *volatile %s_mailbox = 0;\n", object->name);
        fprintf(fp, "static int %s_refCnt = 0;\n", object->name);
        fprintf(fp, "static int %s_cogid = -1;\n", object->name);
        
        fprintf(fp, "\nstatic uint8_t spinBinary[] = {\n");
        for (i = 4, cnt = 0; i < binarySize; ++i) {
            fprintf(fp, " 0x%02x,", binary[i]);
            if (++cnt == 16) {
                putc('\n', fp);
                cnt = 0;
            }
        }
        if (cnt > 0)
            putc('\n', fp);
        fprintf(fp, "};\n");
        
        hdr = (SpinHdr *)binary;
        obj = (SpinObj *)(binary + hdr->pbase);
        off = hdr->pbase + obj->methodcnt * 4 + FIELDOFFSET(SpinObjectPtr, vars);
        dat = hdr->pbase + (obj->methodcnt + obj->objectcnt) * 4;

        fprintf(fp, "\ntypedef struct {\n");
        fprintf(fp, "  uint16_t unused;\n");
        fprintf(fp, "  uint16_t pbase;\n");
        fprintf(fp, "  uint16_t vbase;\n");
        fprintf(fp, "  uint16_t dbase;\n");
        fprintf(fp, "  uint16_t pcurr;\n");
        fprintf(fp, "  uint16_t dcurr;\n");
        fprintf(fp, "} Params;\n");
        
        fprintf(fp, "\nvoid init_%s(%s *obj)\n", object->name, object->name);
        fprintf(fp, "{\n");
        fprintf(fp, "  memset(obj->variables, 0, sizeof(obj->variables));\n");
        fprintf(fp, "  if (%s_cogid < 0) {\n", object->name);
        fprintf(fp, "    Params *params = (Params *)spinBinary;\n");
        fprintf(fp, "    uint16_t *dbase;\n");
        fprintf(fp, "    uint32_t *dat;\n");
        fprintf(fp, "    params->pbase += (uint16_t)(uint32_t)spinBinary - 4;\n");
        fprintf(fp, "    params->vbase  = (uint16_t)(uint32_t)obj->variables;\n");
        fprintf(fp, "    params->dbase  = (uint16_t)(uint32_t)(%s_stack + 2);\n", object->name);
        fprintf(fp, "    params->pcurr += (uint16_t)(uint32_t)spinBinary - 4;\n");
        fprintf(fp, "    params->dcurr  = params->dbase + %d;\n", hdr->dcurr - hdr->dbase);
        fprintf(fp, "    dbase = (uint16_t *)(uint32_t)params->dbase;\n");
        fprintf(fp, "    dbase[-4] = 2;          // pbase + abort-trap + return-value\n");
        fprintf(fp, "    dbase[-3] = 0;          // vbase (not used)\n");
        fprintf(fp, "    dbase[-2] = 0;          // dbase (not used)\n");
        fprintf(fp, "    dbase[-1] = 0xfff9;     // return address\n");
        fprintf(fp, "    *(uint32_t *)dbase = 0; // result\n");
        fprintf(fp, "    dat = (uint32_t *)(spinBinary + 0x%04x);\n", dat - 4);
        fprintf(fp, "    *dat = (uint32_t)&%s_mailbox;\n", object->name);
        fprintf(fp, "    %s_rootVars = (uint8_t *)(uint32_t)params->vbase;\n", object->name);
        fprintf(fp, "    %s_pVarOffset = (uint16_t *)(spinBinary + 0x%04x);\n", object->name, off - 4);
        fprintf(fp, "    %s_cogid = cognew(SPINVM, spinBinary);\n", object->name);
        fprintf(fp, "  }\n");
        fprintf(fp, "  ++%s_refCnt;\n", object->name);
        fprintf(fp, "}\n");
        
        fprintf(fp, "\nvoid term_%s(%s *obj)\n", object->name, object->name);
        fprintf(fp, "{\n");
        fprintf(fp, "  if (%s_refCnt > 0) {\n", object->name);
        fprintf(fp, "    if (--%s_refCnt == 0 && %s_cogid >= 0) {\n", object->name, object->name);
        fprintf(fp, "      cogstop(%s_cogid);\n", object->name);
        fprintf(fp, "      %s_cogid = -1;\n", object->name);
        fprintf(fp, "    }\n");
        fprintf(fp, "  }\n");
        fprintf(fp, "}\n");
        
        while (method != NULL) {
            MethodArg *argument = method->arguments;
            int i;
            
            fprintf(fp, "\nuint32_t %s_%s(%s *obj", object->name, method->name, object->name);
            while (argument != NULL) {
                fprintf(fp, ", uint32_t %s", argument->name);
                argument = argument->next;
            }
            fprintf(fp, ")\n");
            fprintf(fp, "{\n");
            fprintf(fp, "  volatile uint32_t params[%d];\n", 2 + method->argumentCount);
            fprintf(fp, "  params[0] = (uint32_t)obj->variables;\n");
            fprintf(fp, "  params[1] = %d;\n", method->index);
            argument = method->arguments;
            i = 2;
            while (argument != NULL) {
                fprintf(fp, "  params[%d] = %s;\n", i, argument->name);
                argument = argument->next;
                ++i;
            }
            fprintf(fp, "  *%s_pVarOffset = obj->variables - %s_rootVars;\n", object->name, object->name);
            fprintf(fp, "  %s_mailbox = params;\n", object->name);
            fprintf(fp, "  while (%s_mailbox)\n", object->name);
            fprintf(fp, "    ;\n");
            fprintf(fp, "  return params[0];\n");
            fprintf(fp, "}\n");
            
            method = method->next;
        }
        fclose(fp);
    }
}

static void WriteCppStubs(char *file, Object *object, uint8_t *binary, int binarySize)
{
    FILE *fp;
    if ((fp = fopen(file, "w")) != NULL) {
        Method *method = object->methods;
        SpinHdr *hdr;
        SpinObj *obj;
        uint16_t dat, off;
        int cnt, i;
        
        fprintf(fp, "/* This file was generated by spinwrap. Do not edit! */\n");

        fprintf(fp, "\n#include <propeller.h>\n");
        fprintf(fp, "#include <string.h>\n");
        fprintf(fp, "#include \"%s.h\"\n", object->name);
        
        fprintf(fp, "\n#define SPINVM 0xf004\n");
        
        fprintf(fp, "\nuint8_t *%s::m_rootVars;\n", object->name);
        fprintf(fp, "uint16_t *%s::m_pVarOffset;\n", object->name);
        fprintf(fp, "uint32_t %s::m_stack[];\n", object->name);
        fprintf(fp, "volatile uint32_t *volatile %s::m_mailbox = 0;\n", object->name);
        fprintf(fp, "int %s::m_refCnt = 0;\n", object->name);
        fprintf(fp, "int %s::m_cogid = -1;\n", object->name);
        
        fprintf(fp, "\nstatic uint8_t spinBinary[] = {\n");
        for (i = 4, cnt = 0; i < binarySize; ++i) {
            fprintf(fp, " 0x%02x,", binary[i]);
            if (++cnt == 16) {
                putc('\n', fp);
                cnt = 0;
            }
        }
        if (cnt > 0)
            putc('\n', fp);
        fprintf(fp, "};\n");
        
        hdr = (SpinHdr *)binary;
        obj = (SpinObj *)(binary + hdr->pbase);
        off = hdr->pbase + obj->methodcnt * 4 + FIELDOFFSET(SpinObjectPtr, vars);
        dat = hdr->pbase + (obj->methodcnt + obj->objectcnt) * 4;

        fprintf(fp, "\ntypedef struct {\n");
        fprintf(fp, "  uint16_t unused;\n");
        fprintf(fp, "  uint16_t pbase;\n");
        fprintf(fp, "  uint16_t vbase;\n");
        fprintf(fp, "  uint16_t dbase;\n");
        fprintf(fp, "  uint16_t pcurr;\n");
        fprintf(fp, "  uint16_t dcurr;\n");
        fprintf(fp, "} Params;\n");
        
        fprintf(fp, "\n%s::%s()\n", object->name, object->name);
        fprintf(fp, "{\n");
        fprintf(fp, "  memset(m_variables, 0, sizeof(m_variables));\n");
        fprintf(fp, "  if (m_cogid < 0) {\n");
        fprintf(fp, "    Params *params = (Params *)spinBinary;\n");
        fprintf(fp, "    uint16_t *dbase;\n");
        fprintf(fp, "    uint32_t *dat;\n");
        fprintf(fp, "    params->pbase += (uint16_t)(uint32_t)spinBinary - 4;\n");
        fprintf(fp, "    params->vbase  = (uint16_t)(uint32_t)m_variables;\n");
        fprintf(fp, "    params->dbase  = (uint16_t)(uint32_t)(m_stack + 2);\n");
        fprintf(fp, "    params->pcurr += (uint16_t)(uint32_t)spinBinary - 4;\n");
        fprintf(fp, "    params->dcurr  = params->dbase + %d;\n", hdr->dcurr - hdr->dbase);
        fprintf(fp, "    dbase = (uint16_t *)(uint32_t)params->dbase;\n");
        fprintf(fp, "    dbase[-4] = 2;          // pbase + abort-trap + return-value\n");
        fprintf(fp, "    dbase[-3] = 0;          // vbase (not used)\n");
        fprintf(fp, "    dbase[-2] = 0;          // dbase (not used)\n");
        fprintf(fp, "    dbase[-1] = 0xfff9;     // return address\n");
        fprintf(fp, "    *(uint32_t *)dbase = 0; // result\n");
        fprintf(fp, "    dat = (uint32_t *)(spinBinary + 0x%04x);\n", dat - 4);
        fprintf(fp, "    *dat = (uint32_t)&m_mailbox;\n");
        fprintf(fp, "    m_rootVars = (uint8_t *)(uint32_t)params->vbase;\n");
        fprintf(fp, "    m_pVarOffset = (uint16_t *)(spinBinary + 0x%04x);\n", off - 4);
        fprintf(fp, "    m_cogid = cognew(SPINVM, spinBinary);\n");
        fprintf(fp, "  }\n");
        fprintf(fp, "  ++m_refCnt;\n");
        fprintf(fp, "}\n");
        
        fprintf(fp, "\n%s::~%s()\n", object->name, object->name);
        fprintf(fp, "{\n");
        fprintf(fp, "  if (m_refCnt > 0) {\n");
        fprintf(fp, "    if (--m_refCnt == 0 && m_cogid >= 0) {\n");
        fprintf(fp, "      cogstop(m_cogid);\n");
        fprintf(fp, "      m_cogid = -1;\n");
        fprintf(fp, "    }\n");
        fprintf(fp, "  }\n");
        fprintf(fp, "}\n");
        
        while (method != NULL) {
            MethodArg *argument = method->arguments;
            int i;
            
            fprintf(fp, "\nuint32_t %s::%s(", object->name, method->name);
            while (argument != NULL) {
                fprintf(fp, "uint32_t %s", argument->name);
                if (argument->next != NULL)
                    fprintf(fp, ", ");
                argument = argument->next;
            }
            fprintf(fp, ")\n");
            fprintf(fp, "{\n");
            fprintf(fp, "  volatile uint32_t params[%d];\n", 2 + method->argumentCount);
            fprintf(fp, "  params[0] = (uint32_t)m_variables;\n");
            fprintf(fp, "  params[1] = %d;\n", method->index);
            argument = method->arguments;
            i = 2;
            while (argument != NULL) {
                fprintf(fp, "  params[%d] = %s;\n", i, argument->name);
                argument = argument->next;
                ++i;
            }
            fprintf(fp, "  *m_pVarOffset = m_variables - m_rootVars;\n");
            fprintf(fp, "  m_mailbox = params;\n");
            fprintf(fp, "  while (m_mailbox)\n");
            fprintf(fp, "    ;\n");
            fprintf(fp, "  return params[0];\n");
            fprintf(fp, "}\n");
            
            method = method->next;
        }
        fclose(fp);
    }
}

static void WriteProxy(char *file, Object *object)
{
    FILE *fp;
    if ((fp = fopen(file, "w")) != NULL) {
        int maxArgs = 0;
        Method *method;
        int i;
        for (method = object->methods; method != NULL; method = method->next)
            if (method->argumentCount > maxArgs)
                maxArgs = method->argumentCount;
        fprintf(fp, "' This file was generated by spinwrap. Do not edit!\n");
        fprintf(fp, "\nOBJ\n");
        fprintf(fp, "  x : \"%s\"\n\n", object->name);
        fprintf(fp, "PUB dispatch | params, object, index");
        for (i = 1; i <= maxArgs; ++i)
            fprintf(fp, ", arg%d", i);
        fprintf(fp, "\n");
        fprintf(fp, "  repeat\n");
        fprintf(fp, "    repeat while (params := long[pMailbox]) == 0\n");
        fprintf(fp, "    longmove(@object, params, 6)\n");
        fprintf(fp, "    case index\n");
        for (method = object->methods; method != NULL; method = method->next) {
            MethodArg *argument = method->arguments;
            int argn = 1;
            fprintf(fp, "      %d: result := x.%s", method->index, method->name);
            if (argument) {
                fprintf(fp, "(");
                while (argument != NULL) {
                    fprintf(fp, "{%s} arg%d", argument->name, argn);
                    if (argument->next != NULL)
                        fprintf(fp, ", ");
                    argument = argument->next;
                    ++argn;
                }
                fprintf(fp, ")");
            }
            fprintf(fp, "\n");
        }
        fprintf(fp, "      other: result := -1\n");
        fprintf(fp, "    long[params] := result\n");
        fprintf(fp, "    long[pMailbox] := 0\n");
        fprintf(fp, "\nDAT\n");
        fprintf(fp, "  pMailbox long 0\n");
        fclose(fp);
    }
}

static void *safe_calloc(size_t size)
{
    void *p = calloc(1, size);
    if (!p) {
        fprintf(stderr, "error: insufficient memory\n");
        exit(1);
    }
    return p;
}

static void rootname(char *path, char *name)
{
    char *start = strrchr(path, '/');
    char *end = strrchr(path, '.');
    int len;
    if (start)
        ++start;
    else
        start = path;
    if (!end)
        end = &path[strlen(path)];
    len = end - start;
    strncpy(name, start, len);
    name[len] = '\0';
}

static int getLine(ParseContext *c)
{
    if (!(c->flags & F_INIT)) {
        int cnt = fread(c->line, 1, 2, c->fp);
        if (cnt == 2) {
            if (c->line[0] == 0xff && c->line[1] == 0xfe)
                c->flags |= F_UTF16LE;
            else if (c->line[0] == 0xfe && c->line[1] == 0xff)
                c->flags |= F_UTF16BE;
            else
                fseek(c->fp, 0, SEEK_SET);
        }
        else
            fseek(c->fp, 0, SEEK_SET);
        c->flags |= F_INIT;
    }
    if (c->flags & F_UTF16LE) {
        uint8_t *p = c->line;
        int ch;
        do {
            ch = getc(c->fp);
            getc(c->fp); // skip the high byte
            if (ch != EOF && p < &c->line[LINE_MAX - 1])
                *p++ = ch;
        } while (ch != EOF && ch != '\r' && ch != '\n');
        *p = '\0';
        if (c->line[0] == '\0' && ch == EOF)
            return 0;
    }
    else if (c->flags & F_UTF16BE) {
        uint8_t *p = c->line;
        int ch;
        do {
            getc(c->fp); // skip the high byte
            ch = getc(c->fp);
            if (ch != EOF && p < &c->line[LINE_MAX - 1])
                *p++ = ch;
        } while (ch != EOF && ch != '\r' && ch != '\n');
        *p = '\0';
        if (c->line[0] == '\0' && ch == EOF)
            return 0;
    }
    else {
        if (!fgets((char *)c->line, sizeof(c->line), c->fp))
            return 0;
    }
    c->linePtr = c->line;
    ++c->lineNumber;
    return 1;
}

static int token(ParseContext *c)
{
    int tkn;
    
    /* check for the end of line */
    if (SkipSpaces(c) == '\0')
        tkn = T_EOL;
        
    /* check for an identifier */
    else if (firstidchar(*c->linePtr)) {
        char *p = c->token;
        while (*c->linePtr != '\0' && (isalnum(*c->linePtr) || *c->linePtr == '_')) {
            if (p < &c->token[TOKEN_MAX - 1])
                *p++ = *c->linePtr;
            ++c->linePtr;
        }
        *p = '\0';
        tkn = T_IDENTIFIER;
    }
    
    /* check for a number (not really adequate for Spin numbers) */
    else if (*c->linePtr == '-' || isdigit(*c->linePtr)) {
        char *p = c->token;
        int sign = 1;
        c->value = 0;
        if (*c->linePtr == '-') {
            *p++ = *c->linePtr++;
            sign = -1;
        }
        while (*c->linePtr != '\0' && isdigit(*c->linePtr)) {
            c->value = c->value * 10 + *c->linePtr - '0';
            if (p < &c->token[TOKEN_MAX - 1])
                *p++ = *c->linePtr;
            ++c->linePtr;
        }
        *p = '\0';
        c->value *= sign;
        tkn = T_NUMBER;
    }
    
    /* otherwise it's a single-character token */
    else {
        tkn = *c->linePtr;
        ++c->linePtr;
    }
    
    /* return the token */
    return tkn;
}
    
/* SkipSpaces - skip leading spaces */
static int SkipSpaces(ParseContext *c)
{
    /* skip leading spaces */
    while (*c->linePtr != '\0' && isspace(*c->linePtr))
        ++c->linePtr;
    return *c->linePtr;
}

/* ReadEntireFile - read an entire file into an allocated buffer */
static uint8_t *ReadEntireFile(char *name, long *pSize)
{
    uint8_t *buf;
    long size;
    FILE *fp;

    /* open the file in binary mode */
    if (!(fp = fopen(name, "rb")))
        return NULL;

    /* get the file size */
    fseek(fp, 0L, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0L, SEEK_SET);
    
    /* allocate a buffer for the file contents */
    if (!(buf = (uint8_t *)malloc(size))) {
        fclose(fp);
        return NULL;
    }
    
    /* read the contents of the file into the buffer */
    if (fread(buf, 1, size, fp) != size) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    
    /* close the file ad return the buffer containing the file contents */
    *pSize = size;
    fclose(fp);
    return buf;
}

static void DumpSpinBinary(uint8_t *binary)
{
    SpinHdr *hdr = (SpinHdr *)binary;
    SpinObj *obj;
    SpinMethodPtr *methods;
    SpinObjectPtr *objects;
    int i;
    
    printf("clkfreq: %d\n",   hdr->clkfreq);
    printf("clkmode: %02x\n", hdr->clkmode);
    printf("chksum:  %02x\n", hdr->chksum);
    printf("pbase:   %04x\n", hdr->pbase);
    printf("vbase:   %04x\n", hdr->vbase);
    printf("dbase:   %04x\n", hdr->dbase);
    printf("pcurr:   %04x\n", hdr->pcurr);
    printf("dcurr:   %04x\n", hdr->dcurr);
    
    obj = (SpinObj *)(binary + hdr->pbase);
    while (1) {
        printf("\nObject %04x\n", (int)obj - (int)hdr);
        printf("  next:  %04x\n", obj->next);
        printf("  methodcnt:   %d\n",   obj->methodcnt);
        printf("  objectcnt:   %d\n",   obj->objectcnt);
        methods = (SpinMethodPtr *)((uint8_t *)obj + sizeof(SpinObj));
        for (i = 0; i < obj->methodcnt - 1; ++i) {
            printf("  Method %d %04x\n", i, (int)&methods[i] - (int)hdr);
            printf("    code:   %04x\n", methods[i].code);
            printf("    locals: %d\n",   methods[i].locals);
        }
        objects = (SpinObjectPtr *)(methods + obj->methodcnt - 1);
        for (i = 0; i < obj->objectcnt; ++i) {
            printf("  Object %d %04x\n", i, (int)&objects[i] - (int)hdr);
            printf("    object: %04x\n", objects[i].object);
            printf("    vars:   %04x\n", objects[i].vars);
        }
        if (obj->next == 0)
            break;
        obj = (SpinObj *)((uint8_t *)obj + obj->next);
    }
}
