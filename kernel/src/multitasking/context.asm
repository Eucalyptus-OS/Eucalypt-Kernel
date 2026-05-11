global thread_trampoline

thread_trampoline:
    sti
    call rbx

.hang:
    cli
    hlt
    jmp .hang