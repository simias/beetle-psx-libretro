NAME = dynarec-test

CFLAGS = -Wall -g -MMD -MP -DDYNAREC_ARCH_AMD64 -Wno-unused-function -DDYNAREC_DEBUG

AFLAGS = -Wall

SRC = dynarec-tests.c dynarec.c dynarec-compiler.c rbtree.c dynarec-amd64.c
ASM = dynarec-amd64-helpers.s

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
	$(AS) -c -o $@ $<

.PHONY : test
test:   dynarec-test
	./dynarec-test

.PHONY : clean
clean:
	$(info CLEAN $(NAME))
	rm -f $(OBJ) $(DEP)

# Be verbose if V is set
$V.SILENT:
