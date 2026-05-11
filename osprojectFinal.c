/*
   Thread Scheduler for Educational OS  

 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*  CONFIGURATION GLOBALS */

char inputFilePath[512] = {0};
char rfilePath[512]     = {0};
char schedulerType[16]  = "FCFS";

int timeQuantum         = 10000;
int maxPrio             = 4;
int printTransition     = 0;
int printAddRmEvent     = 0;
int printReadyQ         = 0;
int printPreemptProcess = 0;
int printGantt          = 0;

int numDone    = 0;
int ofs        = 1;
int IOBusy     = 0;
int blockStart = 0;
int blockEnd   = 0;

/*   STATE ENUM */
 
 
typedef enum {
    READY = 0, PREEMPT = 1, RUNNG = 2,
    BLOCK = 3, Done = 4, CREATED = 5
} StateEnum;

const char* getState(int s) {
    switch (s) {
        case READY:   return "READY";
        case PREEMPT: return "PREEMPT";
        case RUNNG:   return "RUNNG";
        case BLOCK:   return "BLOCK";
        case Done:    return "Done";
        case CREATED: return "CREATED";
        default:      return "Done";
    }
}

/*  RANDOM NUMBER SUPPORT */

int* randVals  = NULL;
int  randCount = 0;

void readRandom(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "Cannot open rand file: %s\n", path); exit(1); }
    int cap = 1024, n = 0, v;
    randVals = malloc(cap * sizeof(int));
    while (fscanf(f, "%d", &v) == 1) {
        if (n == cap) { cap *= 2; randVals = realloc(randVals, cap * sizeof(int)); }
        randVals[n++] = v;
    }
    fclose(f);
    randCount = n;
}

int assignRandom(int burst) {
    if (burst <= 0) return 1;
    if (ofs >= randCount) ofs = 1;
    int r = 1 + (randVals[ofs] % burst);
    ofs++;
    return r;
}

/*  CONTEXT   */

typedef struct {
    int  programCounter;
    int  stackPointer;
    int  reg[8];
    int  remainingBurst;
    int  remainingTC;
} CPUContext;

/* TIMER INTERRUPT  */

typedef struct {
    int  lastFiredAt;
    int  interruptCount;
    int  preemptionsCount;
} TimerState;

static TimerState timer = {0, 0, 0};

void timerInterruptFired(int currentTime, int pid) {
    timer.lastFiredAt = currentTime;
    timer.interruptCount++;
    (void)pid;
}

int timerInterruptHandler(int currentTime, int runStart) {
    int elapsed = currentTime - runStart;
    if (elapsed >= timeQuantum) {
        timer.preemptionsCount++;
        return 1;
    }
    return 0;
}

/* PCB - Process Control Block */
 
typedef struct PCB {
    int PID, AT, TC, TCInit, CB, CPUBurst, IB, IOBurst;
    int staticPriority, dynamicPriority;
    StateEnum state;
    int stateTimeStamp, nextEventTime, runStart;
    int startTime, finishTime, turnAroundTime, IOTime, CPUWaitTime;
    CPUContext ctx;
    int waitingSince;
    int agingBoosts;
    int assignedCPU;
} PCB;

PCB* newPCB(int pid, int at, int tc, int cb, int ib, int sp) {
    PCB* p = malloc(sizeof(PCB));
    memset(p, 0, sizeof(PCB));
    p->PID = pid; p->AT = at; p->TC = tc; p->TCInit = tc;
    p->CB = cb; p->IB = ib;
    p->staticPriority  = sp;
    p->dynamicPriority = sp - 1;
    p->state           = CREATED;
    p->stateTimeStamp  = at;
    p->waitingSince    = at;
    p->assignedCPU     = -1;
    p->ctx.programCounter = 0;
    p->ctx.stackPointer   = 1024;
    p->ctx.remainingBurst = 0;
    p->ctx.remainingTC    = tc;
    return p;
}

void saveContext(PCB* p, int currentTime) {
    p->ctx.remainingBurst = p->CPUBurst;
    p->ctx.remainingTC    = p->TC;
    p->ctx.programCounter = currentTime;
    p->ctx.stackPointer  -= (currentTime - p->runStart);
    int i;
    for (i = 0; i < 8; i++)
        p->ctx.reg[i] = (p->PID * 100 + i + currentTime) & 0xFFFF;
}

void restoreContext(PCB* p, int currentTime) {
 
    (void)p; (void)currentTime;
}

/* 
  LOAD BALANCING  
*/
#define NUM_CPUS 4

typedef struct {
    int id;
    int load;
    int totalRan;
} VirtualCPU;

static VirtualCPU cpuCores[NUM_CPUS];

void initLoadBalancer(void) {
    int i;
    for (i = 0; i < NUM_CPUS; i++) {
        cpuCores[i].id = i;
        cpuCores[i].load = 0;
        cpuCores[i].totalRan = 0;
    }
}

int assignCPU(PCB* p) {
    int best = 0, i;
    for (i = 1; i < NUM_CPUS; i++)
        if (cpuCores[i].load < cpuCores[best].load) best = i;
    cpuCores[best].load++;
    cpuCores[best].totalRan++;
    p->assignedCPU = best;
    return best;
}

void releaseCPU(PCB* p) {
    if (p->assignedCPU >= 0 && p->assignedCPU < NUM_CPUS) {
        if (cpuCores[p->assignedCPU].load > 0)
            cpuCores[p->assignedCPU].load--;
    }
}

void printLoadBalancerStats(void) {
    int i;
    printf("\n--- Load Balancer Stats (%d virtual CPUs) ---\n", NUM_CPUS);
    for (i = 0; i < NUM_CPUS; i++)
        printf("  CPU %d: %d processes ran\n", cpuCores[i].id, cpuCores[i].totalRan);
}

/* 
 GANTT CHART  
 */
#define MAX_GANTT 4096

typedef struct { int pid, start, end; } GanttSlot;
static GanttSlot gantt[MAX_GANTT];
static int       ganttN = 0;

void ganttRecord(int pid, int start, int end) {
    if (!printGantt) return;
    if (ganttN < MAX_GANTT) {
        gantt[ganttN].pid   = pid;
        gantt[ganttN].start = start;
        gantt[ganttN].end   = end;
        ganttN++;
    }
}

#define GANTT_SCALE 2
#define GANTT_WIDTH 80

void printGanttChart(int totalTime, int numProcesses) {
    int i, t, p;
    int* cpuMap = calloc(totalTime + 1, sizeof(int));
    for (t = 0; t <= totalTime; t++) cpuMap[t] = -1;
    for (i = 0; i < ganttN; i++)
        for (t = gantt[i].start; t < gantt[i].end && t <= totalTime; t++)
            cpuMap[t] = gantt[i].pid;

    printf("\n+----------------------------------------------------------+\n");
    printf("|          CPU SCHEDULING - GANTT CHART                   |\n");
    printf("+----------------------------------------------------------+\n\n");

    for (p = 0; p < numProcesses; p++) {
        printf("P%-3d |", p);
        int col = 0;
        for (t = 0; t <= totalTime && col < GANTT_WIDTH; t += GANTT_SCALE, col++) {
            if (cpuMap[t] == p) printf("#");
            else                printf(" ");
        }
        printf("|\n");
    }

    printf("IDLE |");
    int col = 0;
    for (t = 0; t <= totalTime && col < GANTT_WIDTH; t += GANTT_SCALE, col++)
        printf(cpuMap[t] == -1 ? "." : " ");
    printf("|\n");

    printf("     +");
    for (col = 0; col < GANTT_WIDTH; col++) printf("-");
    printf("+\n");

    int running = 0, idle = 0;
    for (t = 0; t <= totalTime; t++) {
        if (cpuMap[t] >= 0) running++; else idle++;
    }
    printf("\nCPU Busy: %d ticks (%.1f%%)   Idle: %d ticks (%.1f%%)\n",
           running, 100.0*running/(totalTime+1),
           idle,    100.0*idle/(totalTime+1));

    free(cpuMap);
}

/* 
PRIORITY AGING  
  */
#define AGING_INTERVAL 50
static int lastAgingTime = 0;

void agePriorities(PCB** all, int n, int currentTime) {
    if (currentTime - lastAgingTime < AGING_INTERVAL) return;
    lastAgingTime = currentTime;
    int i;
    for (i = 0; i < n; i++) {
        PCB* p = all[i];
        if (p->state == READY) {
            int waited = currentTime - p->waitingSince;
            if (waited >= AGING_INTERVAL) {
                int newPrio = p->dynamicPriority + 1;
                if (newPrio > p->staticPriority - 1)
                    newPrio = p->staticPriority - 1;
                if (newPrio > p->dynamicPriority) {
                    p->dynamicPriority = newPrio;
                    p->agingBoosts++;
                }
            }
        }
    }
}

/* 
 QUEUE  (dynamic circular buffer)
 */
typedef struct {
    PCB** data;
    int   head, tail, cap;
} Queue;

void qInit(Queue* q, int cap) {
	
    q->data = malloc(cap * sizeof(PCB*));
    q->head = q->tail = 0; q->cap = cap;
}
int  qEmpty(const Queue* q)  { 
return q->head == q->tail; 
}
void qPush(Queue* q, PCB* p) {
    int next = (q->tail + 1) % q->cap;
    if (next == q->head) {
        int nc = q->cap * 2;
        PCB** nd = malloc(nc * sizeof(PCB*));
        int i = 0, j = q->head;
        while (j != q->tail) {
		 nd[i++] = q->data[j]; j = (j+1) % q->cap; 
		 }
        free(q->data); q->data = nd; q->head = 0; q->tail = i; q->cap = nc;
    }
    q->data[q->tail] = p; q->tail = (q->tail + 1) % q->cap;
}
PCB* qFront(const Queue* q)  {
 return q->data[q->head]; 
 }
PCB* qPop(Queue* q)          {
 PCB* p = q->data[q->head]; q->head=(q->head+1)%q->cap; return p; 
 }
PCB* qBack(const Queue* q)   {
 return q->data[(q->tail-1+q->cap)%q->cap]; 
 }
PCB* qPopBack(Queue* q)      { 
q->tail=(q->tail-1+q->cap)%q->cap; return q->data[q->tail];
 }

static Queue runQueue;

/* 
 EVENT  +  DES LAYER
 */
typedef struct Event {
    PCB*      evtProcess;
    StateEnum oldState, newState;
    int       evtTimeStamp;
    struct Event* next;
} Event;

Event* newEvent(PCB* p, StateEnum ns, int ts) {
    Event* e = malloc(sizeof(Event));
    e->evtProcess = p; e->oldState = p->state;
    e->newState = ns; e->evtTimeStamp = ts; e->next = NULL;
    return e;
}

typedef struct { Event* head; } DESLayer;
static DESLayer DES;

void desInit(void)           { DES.head = NULL; }
int  desNextEventTime(void)  { return DES.head ? DES.head->evtTimeStamp : -1; }

void desPrintEvents(void) {
    Event* e;
    for (e = DES.head; e; e = e->next)
        printf("%d:%d:%d ", e->evtTimeStamp, e->evtProcess->PID, e->newState);
}

void desAddEvent(Event* ne) {
    if (printAddRmEvent) {
        printf("  AddEvent(%d:%d:%d): ", ne->evtTimeStamp, ne->evtProcess->PID, ne->newState);
        desPrintEvents(); printf(" ==>   ");
    }
    if (!DES.head || ne->evtTimeStamp < DES.head->evtTimeStamp) {
        ne->next = DES.head; DES.head = ne;
    } else {
        Event* cur = DES.head;
        while (cur->next && cur->next->evtTimeStamp <= ne->evtTimeStamp)
            cur = cur->next;
        ne->next = cur->next; cur->next = ne;
    }
    if (printAddRmEvent) { desPrintEvents(); printf("\n"); }
}

Event* desGetEvent(void) {
    if (!DES.head) return NULL;
    Event* e = DES.head; DES.head = e->next; e->next = NULL;
    return e;
}

void desRmEvent(PCB* pcb) {
	
    Event* prev = NULL, *cur = DES.head;
    while (cur) {
    	
        if (cur->evtProcess->PID == pcb->PID) {
            if (printAddRmEvent) { printf("RemoveEvent(%d): ",cur->evtProcess->PID); desPrintEvents(); }
            if (prev) prev->next = cur->next; else DES.head = cur->next;
            free(cur);
            if (printAddRmEvent) { printf(" ==> "); desPrintEvents(); printf("\n"); }
            return;
        }
        prev = cur; cur = cur->next;
    }
}

/* SCHEDULER */
typedef struct Scheduler Scheduler;
struct Scheduler {
    void  (*addProcess)(Scheduler*, PCB*);
    PCB*  (*getNextProcess)(Scheduler*);
    int   (*testPreempt)(Scheduler*, PCB*, PCB*, int);
    void  (*printQueues)(Scheduler*);
    void  (*destroy)(Scheduler*);
};

/* FCFS  */
typedef struct { Scheduler base; Queue q; } FCFSSched;
static void fcfsAdd(Scheduler* s, PCB* p)  { qPush(&((FCFSSched*)s)->q, p); }
static PCB* fcfsNext(Scheduler* s)         { FCFSSched* f=(FCFSSched*)s; return qEmpty(&f->q)?NULL:qPop(&f->q); }
static int  fcfsPreempt(Scheduler* s,PCB* u,PCB* r,int t){ (void)s;(void)u;(void)r;(void)t; return 0; }
static void fcfsPrint(Scheduler* s)        { (void)s; }
static void fcfsFree(Scheduler* s)         { free(((FCFSSched*)s)->q.data); free(s); }

Scheduler* newFCFS(void) {
    FCFSSched* f = malloc(sizeof(FCFSSched)); qInit(&f->q,64);
    f->base.addProcess=fcfsAdd; f->base.getNextProcess=fcfsNext;
    f->base.testPreempt=fcfsPreempt; f->base.printQueues=fcfsPrint; f->base.destroy=fcfsFree;
    return (Scheduler*)f;
}

/* LCFS  */
typedef struct { Scheduler base; Queue q; } LCFSSched;
static void lcfsAdd(Scheduler* s, PCB* p)  { qPush(&((LCFSSched*)s)->q, p); }
static PCB* lcfsNext(Scheduler* s)         { LCFSSched* l=(LCFSSched*)s; return qEmpty(&l->q)?NULL:qPopBack(&l->q); }
static int  lcfsPreempt(Scheduler* s,PCB* u,PCB* r,int t){ (void)s;(void)u;(void)r;(void)t; return 0; }
static void lcfsPrint(Scheduler* s)        { (void)s; }
static void lcfsFree(Scheduler* s)         { free(((LCFSSched*)s)->q.data); free(s); }

Scheduler* newLCFS(void) {
    LCFSSched* l = malloc(sizeof(LCFSSched)); qInit(&l->q,64);
    l->base.addProcess=lcfsAdd; l->base.getNextProcess=lcfsNext;
    l->base.testPreempt=lcfsPreempt; l->base.printQueues=lcfsPrint; l->base.destroy=lcfsFree;
    return (Scheduler*)l;
}

/*  SRTF  */
typedef struct { Scheduler base; PCB** arr; int sz, cap; } SRTFSched;
static void srtfAdd(Scheduler* s, PCB* p) {
    SRTFSched* sr=(SRTFSched*)s;
    if (sr->sz==sr->cap){ sr->cap*=2; sr->arr=realloc(sr->arr,sr->cap*sizeof(PCB*)); }
    sr->arr[sr->sz++]=p;
}
static PCB* srtfNext(Scheduler* s) {
    SRTFSched* sr=(SRTFSched*)s; if (!sr->sz) return NULL;
    int best=0, i;
    for (i=1;i<sr->sz;i++)
        if (sr->arr[i]->TC < sr->arr[best]->TC ||
           (sr->arr[i]->TC==sr->arr[best]->TC && sr->arr[i]->stateTimeStamp<sr->arr[best]->stateTimeStamp))
            best=i;
    PCB* p=sr->arr[best]; sr->arr[best]=sr->arr[--sr->sz]; return p;
}
static int  srtfPreempt(Scheduler* s,PCB* u,PCB* r,int t){ (void)s;(void)u;(void)r;(void)t; return 0; }

static void srtfPrint(Scheduler* s) {
    SRTFSched* sr=(SRTFSched*)s; int i;
    printf("SCHED (%d): ", sr->sz);
    for(i=0;i<sr->sz;i++) printf("%d:%d ",sr->arr[i]->PID,sr->arr[i]->stateTimeStamp);
    printf("\n");
}

static void srtfFree(Scheduler* s){ free(((SRTFSched*)s)->arr); free(s); }

Scheduler* newSRTF(void) {
	
    SRTFSched* sr=malloc(sizeof(SRTFSched)); sr->cap=64; sr->sz=0; sr->arr=malloc(64*sizeof(PCB*));
    sr->base.addProcess=srtfAdd; sr->base.getNextProcess=srtfNext;
    sr->base.testPreempt=srtfPreempt; sr->base.printQueues=srtfPrint; sr->base.destroy=srtfFree;
    return (Scheduler*)sr;
}

/*  RR */
typedef struct { Scheduler base; Queue q; } RRSched;
static void rrAdd(Scheduler* s, PCB* p)  { qPush(&((RRSched*)s)->q, p); }
static PCB* rrNext(Scheduler* s)         { RRSched* r=(RRSched*)s; return qEmpty(&r->q)?NULL:qPop(&r->q); }
static int  rrPreempt(Scheduler* s,PCB* u,PCB* r,int t){ (void)s;(void)u;(void)r;(void)t; return 0; }
static void rrPrint(Scheduler* s)        { (void)s; }
static void rrFree(Scheduler* s)         { free(((RRSched*)s)->q.data); free(s); }

Scheduler* newRR(void) {
	
    RRSched* r=malloc(sizeof(RRSched)); qInit(&r->q,64);
    r->base.addProcess=rrAdd; r->base.getNextProcess=rrNext;
    r->base.testPreempt=rrPreempt; r->base.printQueues=rrPrint; r->base.destroy=rrFree;
    return (Scheduler*)r;
}

/* PRIO / PREPRIO  */

typedef struct {
    Scheduler base;
    Queue* activeQ;
    Queue* expiredQ;
    int isPREPRIO;
} PRIOSched;

static int mqEmpty(Queue* qs, int n) {
    int i; for (i=0;i<n;i++) if (!qEmpty(&qs[i])) return 0; return 1;
}

static void prioAdd(Scheduler* s, PCB* p) {
    PRIOSched* pr=(PRIOSched*)s;
    if (p->dynamicPriority >= 0) {
        qPush(&pr->activeQ[(maxPrio-1)-p->dynamicPriority], p);
    } else {
        p->dynamicPriority = p->staticPriority - 1;
        qPush(&pr->expiredQ[(maxPrio-1)-p->dynamicPriority], p);
    }
}

static PCB* prioNext(Scheduler* s) {
    PRIOSched* pr=(PRIOSched*)s; int i;
    if (mqEmpty(pr->activeQ,maxPrio)) {
        Queue* tmp=pr->activeQ; pr->activeQ=pr->expiredQ; pr->expiredQ=tmp;
        if (printReadyQ) printf("switched queues\n");
    }
    for (i=0;i<maxPrio;i++) if (!qEmpty(&pr->activeQ[i])) return qPop(&pr->activeQ[i]);
    return NULL;
}

static int prioPreempt(Scheduler* s, PCB* u, PCB* r, int ct) {
    PRIOSched* pr=(PRIOSched*)s;
    if (!pr->isPREPRIO) return 0;
    int c1 = u->dynamicPriority > r->dynamicPriority;
    int c2 = r->nextEventTime > ct;
    int res = c1 && c2;
    if (printPreemptProcess)
        printf("    --> PrioPreempt Cond1=%d Cond2=%d (%d) --> %s\n",
               c1, c2, r->nextEventTime-ct, res?"YES":"NO");
    return res;
}
static void prioPrint(Scheduler* s) {
    PRIOSched* pr=(PRIOSched*)s; int i; Queue cp;
    printf("{ ");
    for (i=0;i<maxPrio;i++) {
        if (qEmpty(&pr->activeQ[i])) { printf("[]"); continue; }
        cp = pr->activeQ[i];
        printf("[");
        int first=1;
        while (!qEmpty(&cp)) { if (!first) printf(","); first=0; printf("%d",qPop(&cp)->PID); }
        printf("]");
    }
    printf("}: { ");
    for (i=0;i<maxPrio;i++) {
    	
        if (qEmpty(&pr->expiredQ[i])) { printf("[]"); continue;
		 }
        printf("[%d]",qFront(&pr->expiredQ[i])->PID);
    }
    printf("} :\n");
}
static void prioFree(Scheduler* s) {
    PRIOSched* pr=(PRIOSched*)s; int i;
    for (i=0;i<maxPrio;i++){ free(pr->activeQ[i].data); free(pr->expiredQ[i].data); }
    free(pr->activeQ); free(pr->expiredQ); free(s);
}
Scheduler* newPRIO(int isPREPRIO) {
	
    PRIOSched* pr=malloc(sizeof(PRIOSched)); int i;
    pr->isPREPRIO=isPREPRIO;
    pr->activeQ  = malloc(maxPrio*sizeof(Queue));
    pr->expiredQ = malloc(maxPrio*sizeof(Queue));
    for (i=0;i<maxPrio;i++){ qInit(&pr->activeQ[i],32); qInit(&pr->expiredQ[i],32); }
    pr->base.addProcess    = prioAdd;
    pr->base.getNextProcess= prioNext;
    pr->base.testPreempt   = prioPreempt;
    pr->base.printQueues   = prioPrint;
    pr->base.destroy       = prioFree;
    return (Scheduler*)pr;
}

/*TRANSITION */
void doTransition(PCB* pcb, int currentTime,
                  StateEnum oldState, StateEnum newState) {

    if (oldState == RUNNG && newState == READY) {
        saveContext(pcb, currentTime);
        qPop(&runQueue);
        pcb->CPUBurst -= currentTime - pcb->runStart;
        pcb->TC       -= currentTime - pcb->runStart;
        if (pcb->TC < 0) pcb->TC = 0;
        if (pcb->CPUBurst < 0) pcb->CPUBurst = 0;
        releaseCPU(pcb);
        if (printTransition)
            printf("%d %d %d: %s -> %s  cb=%d rem=%d prio=%d [CTX SAVED cpu=%d]\n",
                   currentTime, pcb->PID, currentTime - pcb->runStart,
                   getState(oldState), getState(newState),
                   pcb->CPUBurst, pcb->TC, pcb->dynamicPriority, pcb->assignedCPU);
        pcb->dynamicPriority--;
        if (!strcmp(schedulerType,"RR")) pcb->dynamicPriority = pcb->staticPriority - 1;
    }

    else if (oldState == READY && newState == RUNNG) {
        restoreContext(pcb, currentTime);
        assignCPU(pcb);
        pcb->CPUWaitTime += currentTime - pcb->stateTimeStamp;
        if (pcb->TC <= pcb->CPUBurst)  pcb->CPUBurst = pcb->TC;
        else if (pcb->CPUBurst == 0)   pcb->CPUBurst = assignRandom(pcb->CB);
        if (pcb->CPUBurst > pcb->TC)   pcb->CPUBurst = pcb->TC;
        if (printTransition)
            printf("%d %d %d: %s -> %s cb=%d rem=%d prio=%d [CTX RESTORED cpu=%d]\n",
                   currentTime, pcb->PID, currentTime - pcb->stateTimeStamp,
                   getState(oldState), getState(newState),
                   pcb->CPUBurst, pcb->TC, pcb->dynamicPriority, pcb->assignedCPU);
    }

    else if (oldState == RUNNG && newState == BLOCK) {
    	
        saveContext(pcb, currentTime);
        pcb->CPUBurst -= currentTime - pcb->runStart;
        pcb->TC       -= currentTime - pcb->runStart;
        if (pcb->TC < 0) pcb->TC = 0;
        if (pcb->CPUBurst < 0) pcb->CPUBurst = 0;
        qPop(&runQueue);
        releaseCPU(pcb);
        if (pcb->TC != 0) {
            if (pcb->IOBurst == 0) pcb->IOBurst = assignRandom(pcb->IB);
            if (printTransition)
                printf("%d %d %d: %s -> %s  ib=%d rem=%d [CTX SAVED]\n",
                       currentTime, pcb->PID, currentTime - pcb->stateTimeStamp,
                       getState(oldState), getState(newState), pcb->IOBurst, pcb->TC);
            if (currentTime > blockEnd) {
                blockStart = currentTime; blockEnd = currentTime + pcb->IOBurst;
                IOBusy += blockEnd - blockStart;
            } else if (currentTime + pcb->IOBurst > blockEnd) {
                IOBusy += currentTime + pcb->IOBurst - blockEnd;
                blockStart = currentTime; blockEnd = currentTime + pcb->IOBurst;
            }
        } else {
            if (printTransition)
                printf("%d %d %d: %s\n",
                       currentTime, pcb->PID, currentTime - pcb->stateTimeStamp, getState(Done));
        }
    }

    else if (oldState == BLOCK && newState == READY) {
        if (printTransition)
            printf("%d %d %d: %s -> %s\n",
                   currentTime, pcb->PID, currentTime - pcb->stateTimeStamp,
                   getState(oldState), getState(newState));
        pcb->IOTime         += currentTime - pcb->stateTimeStamp;
        pcb->dynamicPriority = pcb->staticPriority - 1;
        pcb->IOBurst         = 0;
        pcb->waitingSince    = currentTime;
    }

    else if (newState != Done) {
        if (printTransition)
            printf("%d %d %d: %s -> %s\n",
                   currentTime, pcb->PID, currentTime - pcb->stateTimeStamp,
                   getState(oldState), getState(newState));
    }
}

/* MAIN*/
int main(int argc, char* argv[]) {
    int i;
    for (i = 1; i < argc; i++) {
        char opt[256];
        strncpy(opt, argv[i], 255); opt[255]='\0';

        if (!strcmp(opt,"-v"))  { printTransition    =1; continue; }
        if (!strcmp(opt,"-t"))  { printReadyQ        =1; continue; }
        if (!strcmp(opt,"-e"))  { printAddRmEvent    =1; continue; }
        if (!strcmp(opt,"-p"))  { printPreemptProcess=1; continue; }
        if (!strcmp(opt,"-g"))  { printGantt         =1; continue; }
        if (!strcmp(opt,"-s"))  { continue; }

        char* sched = opt;
        if (opt[0]=='-' && opt[1]=='s') sched = opt+2;
        if (!*sched) continue;

        if (sched[0]=='F') { strcpy(schedulerType,"FCFS");    timeQuantum=10000; continue; }
        if (sched[0]=='L') { strcpy(schedulerType,"LCFS");    timeQuantum=10000; continue; }
        if (sched[0]=='S') { strcpy(schedulerType,"SRTF");    timeQuantum=10000; continue; }
        if (sched[0]=='R') {
            strcpy(schedulerType,"RR");
            if (sched[1]) timeQuantum = atoi(sched+1);
            continue;
        }
        if (sched[0]=='P' || sched[0]=='E') {
            strcpy(schedulerType, sched[0]=='P' ? "PRIO" : "PREPRIO");
            char* rest = sched+1;
            if (*rest) {
                char* colon = strchr(rest,':');
                if (!colon) timeQuantum = atoi(rest);
                else { timeQuantum = atoi(rest); maxPrio = atoi(colon+1); }
            }
            continue;
        }
        if (i == argc-2) { strncpy(inputFilePath, opt, 511); continue; }
        if (i == argc-1) { strncpy(rfilePath,     opt, 511); break;    }
    }

    readRandom(rfilePath);
    qInit(&runQueue, 4);
    desInit();
    initLoadBalancer();

    Scheduler* scheduler;
    if      (!strcmp(schedulerType,"FCFS"))    { scheduler=newFCFS();  timeQuantum=10000; }
    else if (!strcmp(schedulerType,"LCFS"))    { scheduler=newLCFS();  timeQuantum=10000; }
    else if (!strcmp(schedulerType,"SRTF"))    { scheduler=newSRTF();  timeQuantum=10000; }
    else if (!strcmp(schedulerType,"RR"))        scheduler=newRR();
    else if (!strcmp(schedulerType,"PRIO"))      scheduler=newPRIO(0);
    else                                         scheduler=newPRIO(1);

    /*  read input processes  */
    
    FILE* inf = fopen(inputFilePath,"r");
    if (!inf) { fprintf(stderr,"Cannot open input file: %s\n", inputFilePath); return 1; }

    int   processNum = 0, allCap = 64;
    PCB** allPCB = malloc(allCap * sizeof(PCB*));
    int   at, tc, cb, ib;

    while (fscanf(inf,"%d %d %d %d",&at,&tc,&cb,&ib)==4) {
        int prio = assignRandom(maxPrio);
        
        PCB* pcb = newPCB(processNum, at, tc, cb, ib, prio);
        if (processNum==allCap){ allCap*=2; allPCB=realloc(allPCB,allCap*sizeof(PCB*));
		 }
        allPCB[processNum++] = pcb;
        desAddEvent(newEvent(pcb, READY, at));
    }
    fclose(inf);

    /* simulation loop  */
    Event* evt;
    int    callScheduler = 0;
    double totalSimTime  = 0;

    while ((evt = desGetEvent()) != NULL) {
        PCB*      pcb         = evt->evtProcess;
        int       currentTime = evt->evtTimeStamp;
        StateEnum oldState    = pcb->state;
        StateEnum newState    = evt->newState;
        int       preemptRun  = 0;
        free(evt);

        agePriorities(allPCB, processNum, currentTime);

        if (oldState == PREEMPT) oldState = READY;
        if (newState == PREEMPT) {
            timerInterruptFired(currentTime, pcb->PID);
            newState = READY;
        }

        doTransition(pcb, currentTime, oldState, newState);

        switch (newState) {

            case READY: {
                if (oldState == CREATED) pcb->startTime = currentTime;
                if (oldState == CREATED || oldState == BLOCK) {
                    if (!qEmpty(&runQueue)) {
                        PCB* running = qFront(&runQueue);
                        preemptRun = scheduler->testPreempt(scheduler, pcb, running, currentTime);
                        if (preemptRun) {
                            pcb->state = READY; pcb->stateTimeStamp = currentTime;
                            pcb->waitingSince = currentTime;
                            scheduler->addProcess(scheduler, pcb);
                            desRmEvent(running);
                            running->stateTimeStamp = currentTime;
                            running->nextEventTime  = currentTime;
                            desAddEvent(newEvent(running, PREEMPT, currentTime));
                            continue;
                        }
                        
                        pcb->state = READY; pcb->stateTimeStamp = currentTime;
                        pcb->waitingSince = currentTime;
                        scheduler->addProcess(scheduler, pcb);
                        callScheduler = 1;
                        break;
                    }
                }
                pcb->state = READY; pcb->stateTimeStamp = currentTime;
                pcb->waitingSince = currentTime;
                scheduler->addProcess(scheduler, pcb);
                callScheduler = 1;
                break;
            }

            case PREEMPT: {
                qPop(&runQueue);
                pcb->state = READY; pcb->stateTimeStamp = currentTime;
                pcb->waitingSince = currentTime;
                scheduler->addProcess(scheduler, pcb);
                callScheduler = 1;
                break;
            }

            case RUNNG: {
                int minRem      = timeQuantum < pcb->CPUBurst ? timeQuantum : pcb->CPUBurst;
                int nextEvtTime = currentTime + minRem;

                if (timerInterruptHandler(currentTime, pcb->runStart > 0 ? pcb->runStart : currentTime)
                    && minRem > timeQuantum) 
					{
                    minRem = timeQuantum;
                    nextEvtTime = currentTime + minRem;
                }

                pcb->state          = RUNNG;
                pcb->stateTimeStamp = currentTime;
                pcb->runStart       = currentTime;
                pcb->nextEventTime  = nextEvtTime;
                qPush(&runQueue, pcb);

                ganttRecord(pcb->PID, currentTime, nextEvtTime);

                if (pcb->CPUBurst > minRem && pcb->TC > timeQuantum)
                    desAddEvent(newEvent(pcb, PREEMPT, nextEvtTime));
                else
                    desAddEvent(newEvent(pcb, BLOCK,   nextEvtTime));
                break;
            }

            case BLOCK: {
                if (pcb->TC > 0) {
                    pcb->state = BLOCK; pcb->stateTimeStamp = currentTime;
                    desAddEvent(newEvent(pcb, READY, currentTime + pcb->IOBurst));
                    callScheduler = 1;
                    break;
                }
                pcb->state          = Done;
                pcb->stateTimeStamp = currentTime;
                pcb->finishTime     = currentTime;
                pcb->turnAroundTime = pcb->finishTime - pcb->AT;
                doTransition(pcb, currentTime, oldState, Done);
                numDone++;
                
            }
            case Done:
                callScheduler = 1;
                break;

            default: break;
        }

        if (callScheduler) {
            if (desNextEventTime() == currentTime) continue;
            callScheduler = 0;
            if (printReadyQ) scheduler->printQueues(scheduler);
            PCB* nextPCB = scheduler->getNextProcess(scheduler);
            if (nextPCB) desAddEvent(newEvent(nextPCB, RUNNG, currentTime));
        }

        if (numDone == processNum) { totalSimTime = currentTime; break; }
    }

    /* summary output  */
    
    if (!strcmp(schedulerType,"RR")||!strcmp(schedulerType,"PRIO")||!strcmp(schedulerType,"PREPRIO"))
        printf("%s %d\n", schedulerType, timeQuantum);
    else
        printf("%s\n", schedulerType);

    int CPUBusy=0, totalTAT=0, totalWait=0;
    for (i=0; i<processNum; i++) {
        PCB* p = allPCB[i];
        char pid4[8]; sprintf(pid4,"%04d",p->PID);
        printf("%s: %4d %4d %4d %4d %1d |%6d%6d%6d%6d\n",
               pid4, p->AT, p->TCInit, p->CB, p->IB, p->staticPriority,
               p->finishTime, p->turnAroundTime, p->IOTime, p->CPUWaitTime);
        CPUBusy  += p->TCInit;
        totalTAT += p->turnAroundTime;
        totalWait+= p->CPUWaitTime;
    }

    if (totalSimTime > 0) {
        double CPUtil    = 100.0*CPUBusy   /totalSimTime;
        double IOUtil    = 100.0*IOBusy    /totalSimTime;
        double avgTAT    = totalTAT /(double)processNum;
        double avgWait   = totalWait/(double)processNum;
        double throughput= 100.0*processNum/totalSimTime;
        printf("SUM: %.0f %.2f %.2f %.2f %.2f %.3f\n",
               totalSimTime, CPUtil, IOUtil, avgTAT, avgWait, throughput);
    }

    printf("\n--- Timer Interrupt Stats ---\n");
    
    printf("  Total interrupts fired : %d\n", timer.interruptCount);
    printf("  Preemptions caused     : %d\n", timer.preemptionsCount);
    printf("  Last interrupt at      : %d\n", timer.lastFiredAt);
    

    printf("\n--- Priority Aging Stats ---\n");
    for (i=0; i<processNum; i++)
        if (allPCB[i]->agingBoosts > 0)
            printf("  P%d received %d priority boost(s)\n",
                   allPCB[i]->PID, allPCB[i]->agingBoosts);

    printLoadBalancerStats();

    if (printGantt)
        printGanttChart((int)totalSimTime, processNum);

    printf("\n--- Context Switch Summary ---\n");
    printf("  Context saves/restores happen on every RUNNG->READY/BLOCK transition.\n");
    printf("  Each PCB stores: PC, SP, 8 GP registers, remainingBurst, remainingTC.\n");

    scheduler->destroy(scheduler);
    for (i=0;i<processNum;i++) free(allPCB[i]);
    free(allPCB);
    free(randVals);
    free(runQueue.data);
    return 0;
}


