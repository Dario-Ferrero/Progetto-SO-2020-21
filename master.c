#include "common.h"
#include "master.h"

Cell *city_grid;
TaxiStats *tstats;
int sem_id, shm_id, statsq_id,
    taxis_size, taxis_i,
    tstats_size, tstats_i,
    *sources_pos;
struct sembuf sops;

pid_t *taxis, *sources, printer;

int main(int argc, char *argv[])
{
    pid_t child_pid;
    int i, status, pos;
    struct sigaction sa;
    sigset_t sig_mask;

    fprintf(stderr, "Lettura parametri... ");
    read_params();
    fprintf(stderr, "parametri letti.\n");

    fprintf(stderr, "Controllo parametri... ");
    check_params();
    fprintf(stderr, "parametri validi.\n");

    fprintf(stderr, "Inizializzazione griglia... \n\n");
    init_city_grid();
    fprintf(stderr, "Griglia inizializzata\n");

    print_grid();

    fprintf(stderr, "Assegnamento celle sorgente... ");
    assign_sources();
    fprintf(stderr, "celle sorgente assegnate.\n");

    print_grid();

    fprintf(stderr, "Inizializzazione semafori... ");
    init_sems();
    fprintf(stderr, "semafori inizializzati.\n");

    fprintf(stderr, "Creazione coda di messaggi per statistiche taxi... ");
    if ((statsq_id = msgget(getpid(), IPC_CREAT | IPC_EXCL | 0600)) == -1) {
        TEST_ERROR;
        terminate();
    }
    tstats_i = 0;
    tstats_size = SO_TAXI;
    tstats = (TaxiStats *)calloc(tstats_size, sizeof(*tstats));
    fprintf(stderr, "coda creata.\n\n");

    sigemptyset(&sig_mask);
    sigaddset(&sig_mask, SIGALRM);
    bzero(&sa, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);

    fprintf(stderr, "Creazione processi sorgente... ");
    create_sources();

    /* Aspetto che le sorgenti abbiano finito di inizializzarsi */

    SEMOP(sem_id, SEM_KIDS, -SO_SOURCES, 0);
    TEST_ERROR;

    fprintf(stderr, "processi sorgente creati.\n");
    fprintf(stderr, "Master sbloccato, le sorgenti si sono inizializzate.\n");

    fprintf(stderr, "\nCreazione processi taxi... ");
    taxis_i = 0;
    taxis_size = SO_TAXI;
    taxis = (int *)calloc(taxis_size, sizeof(*taxis));
    create_taxis(taxis_size);

    /* Aspetto che i taxi abbiano finito di inizializzarsi */

    SEMOP(sem_id, SEM_KIDS, -SO_TAXI, 0);
    TEST_ERROR;

    fprintf(stderr, "processi taxi creati.\n");
    fprintf(stderr, "Master sbloccato, i taxi si sono inizializzati.\n\n");

    /* print_grid_values(); */

    fprintf(stderr, "Creazione processo printer... ");
    create_printer();

    /* Aspetto che il printer abbia finito di inizializzarsi */

    SEMOP(sem_id, SEM_KIDS, -1, 0);
    TEST_ERROR;

    fprintf(stderr, "Processo printer creato.\n");

    dprintf(STDOUT_FILENO, "Processi figli creati, può partire la simulazione!\n\n");
    dprintf(STDOUT_FILENO, "I Process ID delle sorgenti sono osservabili:\n");
    dprintf(STDOUT_FILENO, "- da terminale, tramite comandi \'top\' oppure \'ps -e | grep sorgente\'\n");
    dprintf(STDOUT_FILENO, "- da applicazione, tramite il Monitor di Sistema\n");
    dprintf(STDOUT_FILENO, "E' possibile generare una richiesta per una sorgente tramite il comando:\n\n");
    dprintf(STDOUT_FILENO, "\t\t\tkill -SIGUSR1 pid_sorgente\n\n");
    dprintf(STDOUT_FILENO, "Premere un tasto qualunque per iniziare la simulazione o Ctrl-c per annullarla.\n");
    scanf("%c", (char *)&i);

    SEMOP(sem_id, SEM_START, -1, 0);
    TEST_ERROR;

    alarm(SO_DURATION);

    /* Ciclo di simulazione */

    while ((child_pid = wait(&status)) != -1) {
        if (WEXITSTATUS(status) != EXIT_TAXI) {
            fprintf(stderr, "Un processo figlio diverso da un taxi è terminato inaspettatamente.\n");
            raise(SIGINT);
        }
        while (msgrcv(statsq_id, &(tstats[tstats_i]), MSG_LEN(tstats[0]),
                        SOURCE_MTYPE, MSG_EXCEPT | IPC_NOWAIT) != -1) {
            sigprocmask(SIG_BLOCK, &sig_mask, NULL);
            if (tstats_i >= tstats_size-1) {
                tstats_size <<= 1;
                tstats = (TaxiStats *)realloc(tstats, tstats_size * sizeof(*tstats));
            }
            tstats_i++;
            create_taxis(1);
            sigprocmask(SIG_UNBLOCK, &sig_mask, NULL);
        }
    }
}


void read_params()
{
    FILE *in;
    char buf[READ_LEN], *token;
    int *params, cnt;

    if ((in = fopen(PARAMS_FILE, "r")) == NULL) {
        TEST_ERROR;
        fprintf(stderr, "Errore nell'apertura del file %s\n", PARAMS_FILE);
        exit(EXIT_FAILURE);
    }

    params = calloc(N_PARAMS, sizeof(*params));
    cnt = 0;
    while (fgets(buf, READ_LEN, in) != NULL) {
        strtok(buf, "=\n");
        token = strtok(NULL, "=\n");
        params[cnt++] = atoi(token);
    }

    SO_HOLES        = params[0];
    SO_TOP_CELLS    = params[1];
    SO_SOURCES      = params[2];
    SO_CAP_MIN      = params[3];
    SO_CAP_MAX      = params[4];
    SO_TAXI         = params[5];
    SO_TIMENSEC_MIN = params[6];
    SO_TIMENSEC_MAX = params[7];
    SO_TIMEOUT      = params[8];
    SO_DURATION     = params[9];

    free(params);
    fclose(in);
}


void check_params()
{
    if (SO_WIDTH <= 0 || SO_HEIGHT <= 0) {
        fprintf(stderr, "Valori di SO_WIDTH e SO_HEIGHT troppo bassi!\n");
        exit(EXIT_FAILURE);
    } else if (SO_SOURCES + SO_HOLES > GRID_SIZE) {
        fprintf(stderr, "Troppi SO_SOURCES e SO_HOLES per la dimensione della griglia!\n");
        exit(EXIT_FAILURE);
    } else if (SO_TOP_CELLS > GRID_SIZE - SO_HOLES) {
        fprintf(stderr, "Troppe SO_TOP_CELLS rispetto alle celle accessibili!\n");
        exit(EXIT_FAILURE);
    } else if (SO_TIMENSEC_MIN > SO_TIMENSEC_MAX) {
        fprintf(stderr, "Valori SO_TIMENSEC incoerenti! (MIN > MAX)\n");
        exit(EXIT_FAILURE);
    } else if (SO_CAP_MIN > SO_CAP_MAX) {
        fprintf(stderr, "Valori SO_CAP incoerenti! (MIN > MAX)\n");
        exit(EXIT_FAILURE);
    } else if (SO_TAXI > (GRID_SIZE - SO_HOLES) * SO_CAP_MIN) {
        fprintf(stderr, "I SO_TAXI potrebbero essere troppi per questa configurazione.\n");
    } else if (GRID_SIZE / SO_HOLES < 9) {
        fprintf(stderr, "Troppi SO_HOLES rispetto alle dimensioni della griglia.\n");
        exit(EXIT_FAILURE);
    }
}


void init_city_grid()
{
    int i, pos;

    if ((shm_id = shmget(getpid(),
                  GRID_SIZE * sizeof(*city_grid),
                  IPC_CREAT | IPC_EXCL | 0666)) == -1) {
        TEST_ERROR;
        fprintf(stderr, "Oggetto IPC (memoria condivisa) già esistente con chiave %d\n", getpid());
        exit(EXIT_FAILURE);
    }

    city_grid = (Cell *)shmat(shm_id, NULL, 0);
    TEST_ERROR;

    fprintf(stderr, "Assegnamento dei %d HOLE... ", SO_HOLES);
    srand(getpid() + time(NULL));
    i = SO_HOLES;
    while (i > 0) {
        pos = rand() % GRID_SIZE;
        if (!check_adj_cells(pos)) {
            city_grid[pos].flags |= HOLE_CELL;
            i--;
        }
    }
    fprintf(stderr, "assegnati (%d rimanenti).\n", i);

    fprintf(stderr, "Inizializzazione delle celle... ");
    for (i = 0; i < GRID_SIZE; i++) {
        if (!IS_HOLE(city_grid[i])) {
            city_grid[i].cross_time = (long)RAND_RNG(SO_TIMENSEC_MIN, SO_TIMENSEC_MAX);
            city_grid[i].msq_id = 0;
            city_grid[i].cross_n = 0;
            city_grid[i].capacity = RAND_RNG(SO_CAP_MIN, SO_CAP_MAX);
            city_grid[i].flags = 0;
        }
    }
    fprintf(stderr, "celle inizializzate.\n\n");
}


int check_adj_cells(long pos)
{
    int x, x_beg, x_end, y, y_beg, y_end;
    int res;
    
    x = pos % SO_WIDTH;
    y = (pos - x) / SO_WIDTH;
    x_beg = x ? x-1 : x;
    y_beg = y ? y-1 : y;
    x_end = (x == SO_WIDTH-1)  ? x : x+1;
    y_end = (y == SO_HEIGHT-1) ? y : y+1;

    res = FALSE;
    for (y = y_beg; !res && y <= y_end; y++) {
        for (x = x_beg; !res && x <= x_end; x++) {
            res = city_grid[INDEX(x, y)].flags & HOLE_CELL;
        }
    }

    return res;
}


void assign_sources()
{
    int cnt, i, pos;

    sources_pos = (int *)calloc(SO_SOURCES, sizeof(*sources_pos));
    cnt = SO_SOURCES;
    i = 0;
    do {
        pos = RAND_RNG(0, GRID_SIZE-1);
        if (!IS_HOLE(city_grid[pos]) && !IS_SOURCE(city_grid[pos])) {
            city_grid[pos].flags |= SRC_CELL;
            sources_pos[i++] = pos;
            cnt--;
        }
    } while (cnt > 0);
}


void init_sems()
{
    int i;

    if ((sem_id = semget(getpid(), NSEMS, IPC_CREAT | IPC_EXCL | 0666)) == -1) {
        TEST_ERROR;
        fprintf(stderr, "Oggetto IPC (array di semafori) già esistente con chiave %d\n", getpid());
        exit(EXIT_FAILURE);
    }
    for (i = 0; i < GRID_SIZE; i++) {
        if (!IS_HOLE(city_grid[i])) {
            semctl(sem_id, i, SETVAL, city_grid[i].capacity);
        } else {
            semctl(sem_id, i, SETVAL, 0);
        }
    }
    semctl(sem_id, SEM_START, SETVAL, 1);
    semctl(sem_id, SEM_KIDS, SETVAL, 0);
    semctl(sem_id, SEM_PRINT, SETVAL, 0);
}


void create_sources()
{
    int i, child_pid;
    char pos_buf[BUF_SIZE],
         nreqs_buf[BUF_SIZE],
        *src_args[4];

    sources = (pid_t *)calloc(SO_SOURCES, sizeof(*sources));
    src_args[0] = SRC_FILE;
    sprintf(nreqs_buf, "%d", (SO_TAXI / SO_SOURCES) ? (SO_TAXI / SO_SOURCES) : 1);
    src_args[2] = nreqs_buf;
    src_args[3] = NULL;
    for (i = 0; i < SO_SOURCES; i++) {
        switch (child_pid = fork()) {
        case -1:
            fprintf(stderr, "Fork fallita.\n");
            TEST_ERROR;
            exit(EXIT_FAILURE);
        case 0:
            sprintf(pos_buf, "%d", sources_pos[i]);
            src_args[1] = pos_buf;
            execvp(SRC_FILE, src_args);
            exit(EXIT_FAILURE);
            break;
        default:
            sources[i] = child_pid;
        }
    }
}


void create_taxis(int n_taxis)
{
    int i, pos, child_pid;
    char *taxi_args[5],
         pos_buf[BUF_SIZE],
         srcs_buf[BUF_SIZE],
         timeout_buf[BUF_SIZE];

    taxi_args[0] = TAXI_FILE;
    sprintf(srcs_buf, "%d", SO_SOURCES);
    taxi_args[2] = srcs_buf;
    sprintf(timeout_buf, "%d", SO_TIMEOUT);
    taxi_args[3] = timeout_buf;
    taxi_args[4] = NULL;
    for (i = 0; i < n_taxis; i++) {
        switch (child_pid = fork()) {
        case -1:
            fprintf(stderr, "Fork fallita.\n");
            TEST_ERROR;
            exit(EXIT_FAILURE);
        case 0:
            /* taxi : genero posizione random e verifico se posso accedere su semaforo */
            pos = -1;
            srand(getpid() + time(NULL));
            while (pos < 0) {
                pos = RAND_RNG(0, GRID_SIZE-1);
                i = semctl(sem_id, pos, GETVAL);
                if (i > 0) {
                    SEMOP(sem_id, pos, -1, 0);
                } else {
                    pos = -1;
                }
            }
            sprintf(pos_buf, "%d", pos);
            taxi_args[1] = pos_buf;
            execvp(TAXI_FILE, taxi_args);
            exit(EXIT_FAILURE);
            break;
        default:
            if (taxis_i >= taxis_size-1) {
                taxis_size <<= 1;
                taxis = (pid_t *)realloc(taxis, taxis_size * sizeof(*taxis));
            }
            taxis[taxis_i++] = child_pid;
        }
    }
}


void create_printer()
{
    int child_pid;
    char *prt_args[2];

    prt_args[0] = PRINTER_FILE;
    prt_args[1] = NULL;
    switch (child_pid = fork()) {
    case -1:
        fprintf(stderr, "Fork fallita.\n");
        TEST_ERROR;
        exit(EXIT_FAILURE);
        break;
    case 0:
        execvp(PRINTER_FILE, prt_args);
        exit(EXIT_FAILURE);
        break;
    default:
        printer = child_pid;
    }
}


void print_grid()
{
    int x, y;

    fprintf(stderr, "\n\n\n     ");
    for (x = 0; x < SO_WIDTH; x++) {
        fprintf(stderr, "%d ", x % 10);
    }
    fprintf(stderr, "\n    ");
    for (x = 0; x < SO_WIDTH; x++) {
        fprintf(stderr, "--");
    }
    fprintf(stderr, "-\n");

    for (y = 0; y < SO_HEIGHT; y++) {
        fprintf(stderr, " %d | ", y % 10);
        for (x = 0; x < SO_WIDTH; x++) {
            if (IS_HOLE(city_grid[INDEX(x, y)])) {
                fprintf(stderr, "H ");
            } else if (IS_SOURCE(city_grid[INDEX(x, y)])) {
                fprintf(stderr, "S ");
            } else {
                fprintf(stderr, "%c ", (char)96);
            }
        }
        fprintf(stderr, "|\n");
    }

    fprintf(stderr, "    ");
    for (x = 0; x < SO_WIDTH; x++) {
        fprintf(stderr, "--");
    }
    fprintf(stderr, "-\n\n\n");
}


void print_grid_values()
{
    long i;

    for (i = 0; i < GRID_SIZE; i++) {
        fprintf(stderr, "city_grid[%3ld].cross_time = %ld\n", i, city_grid[i].cross_time);
        fprintf(stderr, "city_grid[%3ld].msq_id = %d\n", i, city_grid[i].msq_id);
        fprintf(stderr, "city_grid[%3ld].cross_n = %d\n", i, city_grid[i].cross_n);
        fprintf(stderr, "city_grid[%3ld].capacity = %d\n", i, city_grid[i].capacity);
        fprintf(stderr, "city_grid[%3ld].flags = %u\n\n", i, city_grid[i].flags);
    }
}


void end_simulation()
{
    int i, child_cnt;
    long req_succ, req_unpicked, req_abrt;
    SourceStats src_stat;

    SEMOP(sem_id, SEM_PRINT, 0, 0);
    kill(printer, SIGTERM);
    waitpid(printer, NULL, 0);
    SEMOP(sem_id, SEM_PRINT, 1, 0);

    req_succ = req_unpicked = req_abrt = 0;
    for (i = 0; i < SO_SOURCES; i++) {
        kill(sources[i], SIGALRM);
    }

    dprintf(STDOUT_FILENO, "%s:%d: Raccolta statistiche sorgenti\n", __FILE__, __LINE__);
    child_cnt = SO_SOURCES;
    while (child_cnt-- && msgrcv(statsq_id, &src_stat, MSG_LEN(src_stat), SOURCE_MTYPE, 0) != -1) {
        dprintf(STDOUT_FILENO, "%d: CHECK #%d\n", __LINE__, child_cnt);
        req_unpicked += src_stat.reqs_unpicked;
    }
    dprintf(STDOUT_FILENO, "%s:%d: Finito raccolta stats sorgenti.\n", __FILE__, __LINE__);
    while (waitpid(-1, NULL, WNOHANG) != 0);


    child_cnt = semctl(sem_id, SEM_KIDS, GETVAL);
    dprintf(STDOUT_FILENO, "%s:%d: Taxi di seconda generazione = %d\n", __FILE__, __LINE__, child_cnt);
    dprintf(STDOUT_FILENO, "%s:%d: Raccolta statistiche taxi\n", __FILE__, __LINE__);
    for (i = 0, child_cnt = 0; i < taxis_i; i++) {
        if (kill(taxis[i], SIGALRM) == 0) {
            child_cnt++;
        }
    }
    PRINT_INT(taxis_i);
    PRINT_INT(tstats_i);
    PRINT_INT(child_cnt);
    do {
        while (msgrcv(statsq_id, &(tstats[tstats_i]), MSG_LEN(tstats[0]),
            SOURCE_MTYPE, MSG_EXCEPT | IPC_NOWAIT) != -1) {
            dprintf(STDOUT_FILENO, "%s:%d: CHECK #%d\n", __FILE__, __LINE__, child_cnt--);
            if (tstats_i >= tstats_size-1) {
                tstats_size <<= 1;
                tstats = (TaxiStats *)realloc(tstats, tstats_size * sizeof(*tstats));
            }
            tstats_i++;
        }
    } while (wait(NULL) != -1);
    dprintf(STDOUT_FILENO, "%s:%d: Finito raccolta stats taxi.\n", __FILE__, __LINE__);


    for (i = 0; i < taxis_i; i++) {
        if (tstats[i].mtype == REQ_ABRT_MTYPE) {
            req_abrt++;
        }
        req_succ += tstats[i].reqs_compl;
    }

    dprintf(STDOUT_FILENO, "Simulazione Terminata.\n\n");
    dprintf(STDOUT_FILENO, "# Richieste completate con successo %ld\n", req_succ);
    dprintf(STDOUT_FILENO, "# Richieste inevase %ld\n", req_unpicked);
    dprintf(STDOUT_FILENO, "# Richieste abortite %ld\n\n", req_abrt);

    free(taxis);
    free(sources);
    free(tstats);
    free(sources_pos);
}


void handle_signal(int signum)
{
    switch (signum) {
    case SIGTERM:
    case SIGINT:
        /* Terminazione forzata */
        free(tstats);
        free(sources_pos);
        kill(printer, SIGTERM);
        waitpid(printer, NULL, WNOHANG);
        term_kids(sources, SO_SOURCES);
        term_kids(taxis, taxis_i);
        terminate();
        break;
    case SIGALRM:
        end_simulation();
        terminate();
        break;
    default:
        break;
    }
}


void term_kids(pid_t *kids, int nkids)
{
    int i, status;

    for (i = 0; i < nkids; i++) {
        kill(kids[i], SIGTERM);
    }
    while ((i = wait(&status)) != -1) {
        fprintf(stderr, "Figlio #%5d terminato con exit status %d\n", i, WEXITSTATUS(status));
    }
    free(kids);
}


void terminate()
{
    shmdt(city_grid);
    shmctl(shm_id, IPC_RMID, NULL);
    semctl(sem_id, 0, IPC_RMID);
    msgctl(statsq_id, IPC_RMID, NULL);
    exit(EXIT_SUCCESS);
}