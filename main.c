#define _POSIX_C_SOURCE 200809L // Для CLOCK_MONOTONIC/clock_gettime(); должно быть до системных заголовков

#include <ncurses.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <ctype.h>
#include <stdarg.h>
#include <string.h>

#define MAX_ENEMIES 5               //Макс. врагов одновременно
#define MAX_BULLETS 30              //Макс. пуль одновременно
#define SCREEN_WIDTH 160            //Ширина игрового поля
#define SCREEN_HEIGHT 45            //Высота игрового поля
#define PLAYER_MOVE_STEP 1          //Шаг движения игрока (клеток)
#define PLAYER_SPEED_CPS 35.0f      //Скорость перемещения игрока (клеток/сек)
#define PLAYER_FIRE_COOLDOWN_S 0.12f //Задержка между выстрелами (сек)
#define MOVE_HOLD_GRACE_S 0.45      //Время считывания движения (сек)
#define SHOOT_HOLD_GRACE_S 0.20     //Время считывания стрельбы (сек)
#define ENEMY_MOVE_BASE 20          //Базовая задержка движения врага (кадров)
#define ENEMY_MOVE_MIN 2            //Минимальная задержка движения (кадров)
#define ENEMY_MOVE_WAVE_DIV 5       //Делитель роста скорости по волнам
#define ENEMY_SPAWN_BASE 35         //Базовый интервал спавна (кадров)
#define ENEMY_SPAWN_MIN 10          //Минимальный интервал спавна (кадров)
#define ENEMY_SPAWN_WAVE_STEP 3     //Шаг уменьшения интервала спавна по волнам
#define ENEMY_GLYPH '$'             //Символ отрисовки врага
#define ENEMY_LEN 4                 //Длина врага в символах
#define FRAME_DELAY_MS 16           //Пауза между кадрами (мс)
#define MAX_INPUTS_PER_FRAME 32     //Лимит считываний getch() за кадр
#define ENEMY_SPEED_SLEW_CPS2 6.0f  //Подстройки скорости врагов
#define ENEMIES_PER_WAVE 5          //Врагов на волну (умножается на wave)
#define SCORE_PER_ENEMY 10          //Очков за одного врага

typedef struct {
    int x, y;
    int active;
} Bullet;

typedef struct {
    int x, y;
    int active;
    int direction;
    float xf, yf;
} Enemy;

typedef struct {
    int x, y;
    int health;
    int score;
} Player;

static Bullet bullets[MAX_BULLETS];
static Enemy enemies[MAX_ENEMIES];
static Player player;
static int gameOver = 0;
static int wave = 1;
static int totalEnemiesSpawned = 0;
static int totalEnemiesDefeated = 0;
static int enemySpawnCounter = 0;

static float g_enemySpeedCps = 0.0f;
static float g_enemySpeedTargetCps = 0.0f;

static float g_playerXf = 0.0f;
static int g_playerMoveDir = 0;
static double g_playerMoveHeldUntil = 0.0;

static int g_shootPressed = 0;
static double g_shootHeldUntil = 0.0;
static float g_shootCooldownS = 0.0f;

//Ограничение значения диапазоном [lo, hi].
static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

//Возвращаем приблизительный FPS по FRAME_DELAY_MS.
static float frameRateApprox(void) {
    return 1000.0f / (float)FRAME_DELAY_MS;
}

//Минимальный X для левого края спрайта врага.
static int enemyMinX(void) {
    return 1;
}

//Максимальный X для левого края врага.
static int enemyMaxX(void) {
    return (SCREEN_WIDTH - 2) - (ENEMY_LEN - 1);
}

//Сколько врагов нужно заспавнить в указанной волне.
static int enemiesThisWave(int waveNumber) {
    return waveNumber * ENEMIES_PER_WAVE;
}

//Интервал спавна врагов в кадрах для одной волны.
static int enemySpawnRateFrames(int waveNumber) {
    int spawnRate = ENEMY_SPAWN_BASE - (waveNumber * ENEMY_SPAWN_WAVE_STEP);
    if (spawnRate < ENEMY_SPAWN_MIN) spawnRate = ENEMY_SPAWN_MIN;
    return spawnRate;
}

//Задержка движения врагов в кадрах для одной волны (меньше — быстрее).
static int enemyMoveDelayFrames(int waveNumber) {
    int moveDelayFrames = ENEMY_MOVE_BASE - (waveNumber / ENEMY_MOVE_WAVE_DIV);
    if (moveDelayFrames < ENEMY_MOVE_MIN) moveDelayFrames = ENEMY_MOVE_MIN;
    return moveDelayFrames;
}

//Целевая скорость врагов в клетках/сек для одной волны.
static float enemyTargetSpeedCps(int waveNumber) {
    return frameRateApprox() / (float)enemyMoveDelayFrames(waveNumber);
}

//Деактивируем все пули.
static void clearBullets(void) {
    for (int i = 0; i < MAX_BULLETS; i++) {
        bullets[i].active = 0;
    }
}

//Деактивируем всех врагов.
static void clearEnemies(void) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        enemies[i].active = 0;
    }
}

//Возвращаем монотонное время в секундах.
static double monotonicSeconds(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return (double)time(NULL);
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

//Проверяем попадание точки (x,y) в область врага.
static int pointHitsEnemySprite(int x, int y, const Enemy *enemy) {
    return (y == enemy->y) && (x >= enemy->x) && (x < enemy->x + ENEMY_LEN);
}

//Уменьшаем здоровье игрока и завершаем игру при здоровье == 0.
static void playerTakeHit(void) {
    player.health--;
    if (player.health <= 0) gameOver = 1;
}

//Деактивируем врага и увеличиваем счетчик убранных врагов.
static void enemyDeactivate(int enemyIndex) {
    enemies[enemyIndex].active = 0;
    totalEnemiesDefeated++;
}

//Отрисовка рамку игрового поля.
static void drawBorder(void) {
    mvaddch(0, 0, ACS_ULCORNER);
    for (int i = 1; i < SCREEN_WIDTH - 1; i++) {
        mvaddch(0, i, ACS_HLINE);
    }
    mvaddch(0, SCREEN_WIDTH - 1, ACS_URCORNER);

    for (int i = 1; i < SCREEN_HEIGHT - 1; i++) {
        mvaddch(i, 0, ACS_VLINE);
        mvaddch(i, SCREEN_WIDTH - 1, ACS_VLINE);
    }

    mvaddch(SCREEN_HEIGHT - 1, 0, ACS_LLCORNER);
    for (int i = 1; i < SCREEN_WIDTH - 1; i++) {
        mvaddch(SCREEN_HEIGHT - 1, i, ACS_HLINE);
    }
    mvaddch(SCREEN_HEIGHT - 1, SCREEN_WIDTH - 1, ACS_LRCORNER);
}

//Рисуем информацию (очки/жизни/волна) и подсказку управления.
static void drawHudIfPossible(void) {
    int termMaxX = 0;
    int termMaxY = 0;
    getmaxyx(stdscr, termMaxY, termMaxX);
    (void)termMaxX;

    mvprintw(0, 2, "GALAGA");
    mvprintw(1, 2, " Score: %d ", player.score);
    mvprintw(2, 2, " Health: %d ", player.health);
    mvprintw(3, 2, " Wave: %d ", wave);

    if (termMaxY > SCREEN_HEIGHT) {
        mvprintw(SCREEN_HEIGHT, 2, "A/D: Move | Space: Shoot | Q: Quit");
    }
}

//Отрисовка игрока.
static void drawPlayer(void) {
    mvaddch(player.y, player.x, '^');
}

//Отрисовка всех активных врагов.
static void drawEnemies(void) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies[i].active) continue;
        if (enemies[i].y <= 0 || enemies[i].y >= SCREEN_HEIGHT) continue;
        for (int k = 0; k < ENEMY_LEN; k++) {
            mvaddch(enemies[i].y, enemies[i].x + k, ENEMY_GLYPH);
        }
    }
}

//Отрисовка всех активных пуль.
static void drawBullets(void) {
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!bullets[i].active) continue;
        if (bullets[i].y <= 0 || bullets[i].y >= SCREEN_HEIGHT) continue;
        mvaddch(bullets[i].y, bullets[i].x, '|');
    }
}

//Переключаем волну, когда текущая полностью завершена.
static void advanceWaveIfComplete(void) {
    const int enemiesInWave = enemiesThisWave(wave);
    if (totalEnemiesSpawned < enemiesInWave) return;
    if (totalEnemiesDefeated < totalEnemiesSpawned) return;

    wave++;
    totalEnemiesSpawned = 0;
    totalEnemiesDefeated = 0;
    enemySpawnCounter = 0;

    clearBullets();
    clearEnemies();
}

//Обрабатка попадания пуль по врагам.
static void handleBulletEnemyCollisions(void) {
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!bullets[i].active) continue;

        for (int j = 0; j < MAX_ENEMIES; j++) {
            if (!enemies[j].active) continue;

            if (pointHitsEnemySprite(bullets[i].x, bullets[i].y, &enemies[j])) {
                bullets[i].active = 0;
                enemyDeactivate(j);
                player.score += SCORE_PER_ENEMY;
                break;
            }
        }
    }
}

//Обрабатка столкновения врагов с игроком.
static void handleEnemyPlayerCollisions(void) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies[i].active) continue;

        if (pointHitsEnemySprite(player.x, player.y, &enemies[i])) {
            enemyDeactivate(i);
            playerTakeHit();
        }
    }
}

//Печатаем строку по центру экрана на строке y.
static void mvprintwCentered(int y, const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    int x = (SCREEN_WIDTH - (int)strlen(buf)) / 2;
    if (x < 0) x = 0;
    mvprintw(y, x, "%s", buf);
}

//Нормализация кода клавиши.
static int normalizeKey(int ch) {
    if (ch >= 0 && ch <= 255) {
        return tolower((unsigned char)ch);
    }
    return ch;
}

//Инициализируем состояние игры перед стартом волна№1.
static void initGame(void) {
    player.x = SCREEN_WIDTH / 2;
    player.y = SCREEN_HEIGHT - 2;
    player.health = 3;
    player.score = 0;
    wave = 1;
    gameOver = 0;
    totalEnemiesSpawned = 0;
    totalEnemiesDefeated = 0;
    enemySpawnCounter = 0;

    g_enemySpeedTargetCps = enemyTargetSpeedCps(wave);
    g_enemySpeedCps = g_enemySpeedTargetCps;

    g_playerXf = (float)player.x;
    g_playerMoveDir = 0;
    g_playerMoveHeldUntil = 0.0;
    g_shootPressed = 0;
    g_shootHeldUntil = 0.0;
    g_shootCooldownS = 0.0f;
    
    clearBullets();
    clearEnemies();

    srand(time(NULL));
}

//Добавление нового врага; возвращаем 1 при успехе.
static int spawnEnemy(void) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies[i].active) {
            const int minX = enemyMinX();
            const int maxX = enemyMaxX();
            const int range = maxX - minX + 1;
            if (range <= 0) return 0;
            enemies[i].x = (rand() % range) + minX;
            enemies[i].y = 1;
            enemies[i].xf = (float)enemies[i].x;
            enemies[i].yf = (float)enemies[i].y;
            enemies[i].active = 1;
            enemies[i].direction = (rand() % 2) ? 1 : -1;
            return 1;
        }
    }
    return 0;
}

//Создаем новую пулю над игроком, если есть место.
static void shootBullet(void) {
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!bullets[i].active) {
            bullets[i].x = player.x;
            bullets[i].y = player.y - 1;
            bullets[i].active = 1;
            return;
        }
    }
}

//Обрабатка одного нажатие клавиши и обновение состояния ввода.
static void handleInput(int ch) {
    if (ch == ERR) return;
    
    ch = normalizeKey(ch);
    
    const double nowT = monotonicSeconds();
    switch (ch) {
        case 'a':
        case KEY_LEFT:
            g_playerMoveDir = -1;
            g_playerMoveHeldUntil = nowT + MOVE_HOLD_GRACE_S;
            break;
        
        case 'd':
        case KEY_RIGHT:
            g_playerMoveDir = 1;
            g_playerMoveHeldUntil = nowT + MOVE_HOLD_GRACE_S;
            break;
        
        case ' ':
            g_shootPressed = 1;
            g_shootHeldUntil = nowT + SHOOT_HOLD_GRACE_S;
            break;
        
        case 'q':
            gameOver = 1;
            break;
    }
}

//Обновляем позицию игрока и стрельбу по состоянию ввода.
static void updatePlayer(float dt, double nowT) {
    if (nowT <= g_playerMoveHeldUntil && g_playerMoveDir != 0) {
        g_playerXf += (float)g_playerMoveDir * PLAYER_SPEED_CPS * dt;
    }

    g_playerXf = clampf(g_playerXf, 1.0f, (float)(SCREEN_WIDTH - 2));
    player.x = (int)(g_playerXf + 0.5f);

    g_shootCooldownS -= dt;
    if (g_shootCooldownS < 0.0f) g_shootCooldownS = 0.0f;

    const int shootingHeld = (nowT <= g_shootHeldUntil);
    if ((g_shootPressed || shootingHeld) && g_shootCooldownS <= 0.0f) {
        shootBullet();
        g_shootCooldownS = PLAYER_FIRE_COOLDOWN_S;
    }
    g_shootPressed = 0;
}

//Перемещаем пули вверх и удаляем вышедшие за границу поля.
static void updateBullets(void) {
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (bullets[i].active) {
            bullets[i].y--;
            
            if (bullets[i].y <= 0) {
                bullets[i].active = 0;
            }
        }
    }
}

//Выполняем спавн врагов по таймеру волны.
static void spawnEnemiesThisFrame(void) {
    enemySpawnCounter++;

    const int spawnRate = enemySpawnRateFrames(wave);
    const int enemiesInWave = enemiesThisWave(wave);
    if (enemySpawnCounter < spawnRate) return;
    if (totalEnemiesSpawned >= enemiesInWave) return;

    if (spawnEnemy()) {
        totalEnemiesSpawned++;
        enemySpawnCounter = 0;
    } else {
        enemySpawnCounter = spawnRate;
    }
}

//Считаем смещение врагов за кадр с плавной подстройкой скорости.
static float enemyDeltaCells(float dt) {
    g_enemySpeedTargetCps = enemyTargetSpeedCps(wave);
    const float maxSpeedDelta = ENEMY_SPEED_SLEW_CPS2 * dt;
    const float diff = g_enemySpeedTargetCps - g_enemySpeedCps;
    g_enemySpeedCps += clampf(diff, -maxSpeedDelta, maxSpeedDelta);
    return g_enemySpeedCps * dt;
}

//Двигаем врагов и проверяем выход вниз/попадания по игроку.
static void moveEnemies(float delta) {
    const int minX = enemyMinX();
    const int maxX = enemyMaxX();

    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies[i].active) continue;

        enemies[i].xf += (float)enemies[i].direction * delta;
        enemies[i].yf += delta;

        if (enemies[i].xf <= (float)minX) {
            enemies[i].xf = (float)minX;
            enemies[i].direction = 1;
        } else if (enemies[i].xf >= (float)maxX) {
            enemies[i].xf = (float)maxX;
            enemies[i].direction = -1;
        }

        enemies[i].x = (int)(enemies[i].xf + 0.5f);
        enemies[i].y = (int)(enemies[i].yf + 0.5f);

        if (enemies[i].y >= SCREEN_HEIGHT - 1) {
            enemyDeactivate(i);
            playerTakeHit();
        }
    }
}

//Спавним и двигаем врагов за кадр.
static void updateEnemies(float dt) {
    spawnEnemiesThisFrame();
    moveEnemies(enemyDeltaCells(dt));
}

//Проверяем все типы столкновений и завершаем волны.
static void checkCollisions(void) {
    handleBulletEnemyCollisions();
    handleEnemyPlayerCollisions();
    advanceWaveIfComplete();
}

//Отрисовываем кадр игры.
static void render(void) {
    erase();

    drawBorder();

    drawPlayer();
    drawEnemies();
    drawBullets();
    drawHudIfPossible();
    
    wnoutrefresh(stdscr);
    doupdate();
}

//Главный игровой цикл (ввод -> обновление -> рендер).
static void gameLoop(void) {
    nodelay(stdscr, TRUE);
    wtimeout(stdscr, 0);
    
    double prevT = monotonicSeconds();
    while (!gameOver) {
        const double nowT = monotonicSeconds();
        float dt = (float)(nowT - prevT);
        prevT = nowT;
        dt = clampf(dt, 0.0f, 0.05f);

        for (int i = 0; i < MAX_INPUTS_PER_FRAME; i++) {
            int ch = getch();
            if (ch == ERR) break;
            handleInput(ch);
        }

        updatePlayer(dt, nowT);
        
        updateBullets();
        updateEnemies(dt);
        checkCollisions();
        
        render();

        napms(FRAME_DELAY_MS);
    }
}

//Экран Game Over и выбор перезапуска/выхода.
static int endGame(void) {
    erase();
    
    const int cy = SCREEN_HEIGHT / 2;
    mvprintwCentered(cy - 3, "%s", "GAME OVER!");
    mvprintwCentered(cy - 1, "Final Score: %d", player.score);
    mvprintwCentered(cy,     "Wave Reached: %d", wave);
    mvprintwCentered(cy + 2, "%s", "Press R to restart");
    mvprintwCentered(cy + 3, "%s", "Press Q or ESC to quit");
    wnoutrefresh(stdscr);
    doupdate();
    
    int ch;
    while (1) {
        ch = getch();
        if (ch == 'r' || ch == 'R') return 1; // Перезапуск
        if (ch == 'q' || ch == 'Q' || ch == 27) return 0; // Выход
    }
}

int main(void) {
    initscr();
    raw();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    
    int maxX, maxY;
    getmaxyx(stdscr, maxY, maxX);
    
    if (maxX < SCREEN_WIDTH || maxY < SCREEN_HEIGHT) {
        endwin();
        printf("Требуется размер терминала минимум %dx%d\n", SCREEN_WIDTH, SCREEN_HEIGHT);
        return 1;
    }
    
    while (1) {
        initGame();
        gameLoop();
        if (!endGame()) {
            break;
        }
    }
    
    endwin();           
    return 0;        
}
//
