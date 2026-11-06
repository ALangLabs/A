/* Astra A runtime support: aval ABI for A-language programs.
 * Auto-included as #include by tcc when compiling .a files. */
#ifndef ASTRA_A_RUNTIME_H
#define ASTRA_A_RUNTIME_H
typedef long long a_i64;
typedef struct { a_i64 i; double d; int f; } aval;
static aval a_d(double d){ aval v; v.f=1; v.d=d; v.i=0; return v; }
static aval a_i(a_i64 i){ aval v; v.f=0; v.d=0; v.i=i; return v; }
static aval a_b(int i){ aval v; v.f=4; v.d=0; v.i=i?1:0; return v; }
static int a_truthy(aval v){ return v.f==1 ? v.d!=0.0 : v.i!=0; }
typedef struct a_obj { int type; a_i64 len; a_i64 cap; void *data; } a_obj;
static a_obj *a_new_obj(int t,a_i64 len,a_i64 cap,void*d){ a_obj*o=(a_obj*)malloc(sizeof(a_obj)); if(!o){printf("out of memory\n");exit(1);} o->type=t;o->len=len;o->cap=cap;o->data=d; return o; }
static a_i64 ai(aval v){ return v.f==1 ? (a_i64)v.d : v.i; }
static double ad(aval v){ return v.f==1 ? v.d : (double)v.i; }
#endif
