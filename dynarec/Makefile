NAME = dynarec-test

# Set to 1 to enable GDB debugging, to 2 to also add verbose debug messages
DEBUG = 0
TARGET_ARCH = AMD64

CFLAGS = -Wall -MMD -MP -DDYNAREC_ARCH_$(TARGET_ARCH) -Wno-unused-function
AFLAGS = --warn

SRC = dynarec-tests.c dynarec.c dynarec-compiler.c rbtree.c
ASM =

ifeq ($(TARGET_ARCH),AMD64)
SRC += dynarec-amd64.c
ASM += dynarec-amd64-helpers.s
endif

ifeq ($(DEBUG),1)
CFLAGS += -g -DDEBUG -DDYNAREC_JIT_DEBUGGER
AFLAGS += --gstabs
SRC += dynarec-jit-debugger.c
else ifeq ($(DEBUG),2)
CFLAGS += -g -DDEBUG -DDYNAREC_JIT_DEBUGGER -DDYNAREC_LOG_DEBUG -DDYNAREC_NO_PATCH
AFLAGS += --gstabs
SRC += dynarec-jit-debugger.c
else
CFLAGS += -O2 -DNDEBUG
endif

OBJ = $(SRC:%.c=%.o) $(ASM:%.s=%.o)
DEP = $(SRC:%.c=%.d)

$(NAME) : $(OBJ)
	$(info LD $@)
	$(CC) $(LDFLAGS) -o $@ $^

-include $(DEP)

%.o: %.c
	$(info CC $@)
	$(CC) -c $(CFLAGS) -o $@ $<

%.o: %.s
	$(info AS $@)
	$(AS) -c $(AFLAGS) -o $@ $<

.PHONY : test
test:   dynarec-test
	./dynarec-test

.PHONY : clean
clean:
	$(info CLEAN $(NAME))
	rm -f $(OBJ) $(DEP)

# Be verbose if V is set
$V.SILENT:
