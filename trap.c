#include "types.h"
#include "defs.h"
#include "param.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"
#include "pmap.h"
#include "memlayout.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
int ticks;

void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);
  
  initlock(&tickslock, "time");
}

void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

void
trap(struct trapframe *tf)
{
  uint cr2;
  if (cp!=NULL)
  dbmsg("trap frame from %x %x\n",tf->eip, tf->trapno);
  if(tf->trapno == T_SYSCALL){
    if(cp->killed)
      exit();
    cp->tf = tf;
    syscall();
    if(cp->killed)
      exit();
    return;
  }

  switch(tf->trapno){
  cprintf("interrupt %x, trap frame : %x\n",tf->trapno, (uint)tf);
  case IRQ_OFFSET + IRQ_TIMER:
    if(cpu() == 0){
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapic_eoi();
    break;
  case IRQ_OFFSET + IRQ_IDE:
    ide_intr();
    lapic_eoi();
    break;
  case IRQ_OFFSET + IRQ_KBD:
    kbd_intr();
    lapic_eoi();
    break;
  case IRQ_OFFSET + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpu(), tf->cs, tf->eip);
    lapic_eoi();
    break;
  case T_PGFLT:
    cr2 = rcr2();
    if (cp == 0 || (tf->cs&3) == 0) {
      cprintf("page fault in kernel from cpu %x eip %x cr2 %x",cpu(), tf->eip, cr2);
      panic("trap due to kernel page fault");
    }
    if (pgfault_handler(cr2) < 0) {
      cprintf("can not handler user page fault from cpu %x eip %x cr2 %x",cpu(), tf->eip, cr2);
      panic("trap due to user page fault");
    }
    break;
  default:
    if(cp == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x esp %x cr2 %x \n",
              tf->trapno, cpu(), tf->eip, tf->esp, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d eip %x cr2 %x -- kill proc\n",
            cp->pid, cp->name, tf->trapno, tf->err, cpu(), tf->eip, rcr2());
    cp->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running 
  // until it gets to the regular system call return.)
  if(cp && cp->killed && (tf->cs&3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if(cp && cp->state == RUNNING && tf->trapno == IRQ_OFFSET+IRQ_TIMER)
    yield();
}
