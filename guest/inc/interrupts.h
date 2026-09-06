#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#define BUFFER_SIZE 64

void init_idt(void);
int interrupts_finished(void);

#endif /* INTERRUPTS_H */
