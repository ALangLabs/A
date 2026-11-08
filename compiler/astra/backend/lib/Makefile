#
# ASTRA makefile for libastra1.a
#

TOP = ..
include $(TOP)/Makefile
VPATH = $(TOPSRC)/lib $(TOPSRC)/win32/lib
T = $(or $(CROSS_TARGET),$(NATIVE_TARGET),unknown)
X = $(if $(CROSS_TARGET),$(CROSS_TARGET)-)
XCFG = $(or $(findstring -win,$T),-unx)
S = $(if $(findstring yes,$(SILENT)),@$(info * $@))

ASTRA = $(TOP)/$(X)astra$(EXESUF)
XASTRA ?= $(TOP)/$(X)astra$(EXESUF)
XCC = $(XASTRA)
XAR = $(XASTRA) -ar
XFLAGS-unx = -B$(TOPSRC)
XFLAGS-win = -B$(TOPSRC)/win32 -I$(TOPSRC)/include
XFLAGS = $(XFLAGS$(XCFG))

# in order to use gcc, type: make <target>-libastra1-usegcc=yes
arm-libastra1-usegcc ?= no

ifeq "$($(T)-libastra1-usegcc)" "yes"
 XCC = $(CC)
 XAR = $(AR)
 XFLAGS = $(CFLAGS) -fPIC -fno-omit-frame-pointer -Wno-unused-function -Wno-unused-variable
endif

XFLAGS += -I$(TOP)

I386_O = libastra1.o $(COMMON_O)
X86_64_O = libastra1.o $(COMMON_O)
ARM_O = libastra1.o armeabi.o armflush.o $(COMMON_O)
ARM64_O = lib-arm64.o $(COMMON_O)
RISCV64_O = lib-arm64.o $(COMMON_O)
COMMON_O = stdatomic.o atomic.o builtin.o alloca.o alloca-bt.o
WIN_O = crt1.o crt1w.o wincrt1.o wincrt1w.o dllcrt1.o dllmain.o winex.o
LIN_O = dsohandle.o
OSX_O =

# run/coverage only for native compiler (backtrace/bounds-check removed:
# CONFIG_ASTRA_BACKTRACE/BCHECK are hardcoded 0 in cmake/libastra.cmake)
Nat = $(if $X,no,)

$(Nat)COMMON_O += runmain.o tcov.o

# not in libastra1.a
EXTRA_O = runmain.o

OBJ-i386 = $(I386_O) pic86.o $(LIN_O)
OBJ-x86_64 = $(X86_64_O) va_list.o $(LIN_O)
OBJ-x86_64-osx = $(X86_64_O) va_list.o $(OSX_O)
OBJ-i386-win32 = $(I386_O) chkstk.o $(WIN_O)
OBJ-x86_64-win32 = $(X86_64_O) chkstk.o $(WIN_O)
OBJ-arm64 = $(ARM64_O) armflush.o $(LIN_O)
OBJ-arm64-osx = $(ARM64_O) $(OSX_O)
OBJ-arm64-win32 = $(ARM64_O) chkstk.o $(WIN_O)
OBJ-arm = $(ARM_O) $(LIN_O)
OBJ-arm-fpa = $(OBJ-arm)
OBJ-arm-fpa-ld = $(OBJ-arm)
OBJ-arm-vfp = $(OBJ-arm)
OBJ-arm-eabi = $(OBJ-arm)
OBJ-arm-eabihf = $(OBJ-arm)
OBJ-arm-wince = $(ARM_O) $(WIN_O)
OBJ-riscv64 = $(RISCV64_O) armflush.o $(LIN_O)

OBJ-extra = $(filter $(EXTRA_O),$(OBJ-$T))
OBJ-libastra1 = $(addprefix $(X),$(filter-out $(OBJ-extra),$(OBJ-$T)))

ALL = $(addprefix $(TOP)/,$(X)libastra1.a $(OBJ-extra))

all: $(ALL)

$(TOP)/$(X)libastra1.a : $(OBJ-libastra1) $(ASTRA)
	$S$(XAR) rcs $@ $(OBJ-libastra1)

$(X)%.o : %.c $(ASTRA)
	$S$(XCC) -c $< -o $@ $(XFLAGS)

$(X)%.o : %.S $(ASTRA)
	$S$(XCC) -c $< -o $@ $(XFLAGS)

$(TOP)/%.o : %.c $(ASTRA)
	$S$(XCC) -c $< -o $@ $(XFLAGS)

$(X)crt1w.o : crt1.c
$(X)wincrt1w.o : wincrt1.c

# don't try to make it
$(ASTRA) : ;

clean :
	rm -f *.o $(addprefix $(TOP)/,*libastra1.a $(EXTRA_O))
