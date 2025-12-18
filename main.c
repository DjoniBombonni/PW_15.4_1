// ============================================================
// ГАЛАГА
// Управление: A/D - движение, SPACE - выстрел, Q - выход
// ============================================================

// Нужно для CLOCK_MONOTONIC/clock_gettime() на POSIX системах.
// ДОЛЖНО быть определено до подключения системных заголовков.
#define _POSIX_C_SOURCE 200809L

#include <ncurses.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

// ============================================================
// КОНСТАНТЫ И ОПРЕДЕЛЕНИЯ
// ============================================================

#define MAX_ENEMIES 5        // Максимум врагов одновременно
#define MAX_BULLETS 30        // Максимум пуль одновременно
#define SCREEN_WIDTH 160       // Ширина игрового экрана
#define SCREEN_HEIGHT 45      // Высота игрового экрана
#define PLAYER_MOVE_STEP 1     // Шаг перемещения игрока (можно менять для регулировки скорости)
#define PLAYER_SPEED_CPS 35.0f // Скорость игрока (клеток в секунду)
#define PLAYER_FIRE_COOLDOWN_S 0.12f // Кулдаун выстрела (сек)
#define MOVE_HOLD_GRACE_S 0.45  // Как долго продолжаем движение без автоповтора (сек)
#define SHOOT_HOLD_GRACE_S 0.20 // Как долго считаем пробел «удерживаемым» (сек)
#define ENEMY_MOVE_BASE 20      // Базовая скорость движения врагов (меньше — быстрее)
#define ENEMY_MOVE_MIN 2       // Минимально допустимая скорость движения врагов
#define ENEMY_MOVE_WAVE_DIV 5  // Как быстро растет скорость с волнами (делитель)
#define ENEMY_SPAWN_BASE 35    // Базовая задержка спавна врагов (меньше — чаще)
#define ENEMY_SPAWN_MIN 10     // Минимальная задержка спавна
#define ENEMY_SPAWN_WAVE_STEP 3// Как быстро спавн учащается с волнами

// Вид врага (ширина в символах). Должно быть ровно 4 символа по заданию.
#define ENEMY_SPRITE "$$$$"
#define ENEMY_WIDTH ((int)(sizeof(ENEMY_SPRITE) - 1))

// Настройки «кадра» (влияют на отзывчивость и стабильность)
#define FRAME_DELAY_MS 16      // Пауза между кадрами (примерно 60 FPS)
#define MAX_INPUTS_PER_FRAME 32// Сколько событий клавиатуры обрабатываем за кадр (чтобы ввод не "съедал" обновления)

// Плавность скорости врагов (ускорение/замедление), в "клетках в секунду^2"
// Чем больше значение, тем быстрее скорость догоняет целевую.
#define ENEMY_SPEED_SLEW_CPS2 6.0f

// Параметры волны
#define ENEMIES_PER_WAVE 5     // Сколько врагов нужно заспавнить на одну волну (умножается на wave)

// ============================================================
// СТРУКТУРЫ ДАННЫХ
// ============================================================

// Структура для пули
typedef struct {
    int x, y;               // Координаты пули
    int active;             // Активна ли пуля (1 - да, 0 - нет)
} Bullet;

// Структура для врага
typedef struct {
    int x, y;               // Координаты врага
    int active;             // Активен ли враг (1 - да, 0 - нет)
    int direction;          // Направление движения (-1 влево, 1 вправо)
    float xf, yf;            // Дробная позиция (для более плавного движения)
} Enemy;

// Структура для игрока
typedef struct {
    int x, y;               // Координаты игрока
    int health;             // Жизни (здоровье)
    int score;              // Набранные очки
} Player;

// ============================================================
// ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ
// ============================================================

Bullet bullets[MAX_BULLETS];           // Массив пуль
Enemy enemies[MAX_ENEMIES];            // Массив врагов
Player player;                         // Данные игрока
int gameOver = 0;                      // Флаг завершения игры
int wave = 1;                          // Номер текущей волны
int totalEnemiesSpawned = 0;           // Всего врагов спавнено в волне
int totalEnemiesDefeated = 0;          // Всего врагов "убрано" в волне (убиты + ушли вниз + столкнулись с игроком)
int enemySpawnCounter = 0;             // Счетчик времени спавна врагов

// Текущая и целевая скорости врагов (в клетках/сек). Меняются плавно.
static float g_enemySpeedCps = 0.0f;
static float g_enemySpeedTargetCps = 0.0f;

// Состояние ввода игрока (чтобы движение не "обрывалось" при стрельбе)
static float g_playerXf = 0.0f;
static int g_playerMoveDir = 0; // -1 влево, 1 вправо
static double g_playerMoveHeldUntil = 0.0;

static int g_shootPressed = 0; // нажато в текущем кадре
static double g_shootHeldUntil = 0.0;
static float g_shootCooldownS = 0.0f;

static int g_termMaxX = 0;
static int g_termMaxY = 0;

static float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static double monotonicSeconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// ============================================================
// ВСПОМОГАТЕЛЬНАЯ ФУНКЦИЯ: Нормализация клавиши
// Описание:
// - ncurses возвращает либо ASCII (0..255), либо специальные KEY_* (обычно > 255)
// - tolower() корректно работает только для unsigned char/EOF
//   поэтому KEY_LEFT/KEY_RIGHT нельзя напрямую пропускать через tolower()
// ============================================================
static int normalizeKey(int ch) {
    if (ch >= 0 && ch <= 255) {
        return tolower((unsigned char)ch);
    }
    return ch;
}

// ============================================================
// ФУНКЦИЯ: Инициализация игры
// Описание: Устанавливает начальные значения для игрока и врагов
// ============================================================
void initGame() {
    // Устанавливаем начальную позицию игрока в центре экрана снизу
    player.x = SCREEN_WIDTH / 2;
    player.y = SCREEN_HEIGHT - 2;
    player.health = 3;      // 3 жизни в начале
    player.score = 0;       // 0 очков в начале
    wave = 1;               // Первая волна
    gameOver = 0;           // Сбрасываем флаг завершения игры
    totalEnemiesSpawned = 0;
    totalEnemiesDefeated = 0;
    enemySpawnCounter = 0;  // Сбрасываем таймер спавна врагов

    // Инициализируем скорость врагов под первую волну (без резкого старта)
    // Используем ту же формулу, что и раньше (через "задержку в кадрах"),
    // но переводим ее в клетки/сек.
    const float fpsApprox = 1000.0f / (float)FRAME_DELAY_MS;
    int moveDelayFrames = ENEMY_MOVE_BASE - (wave / ENEMY_MOVE_WAVE_DIV);
    if (moveDelayFrames < ENEMY_MOVE_MIN) moveDelayFrames = ENEMY_MOVE_MIN;
    g_enemySpeedTargetCps = fpsApprox / (float)moveDelayFrames;
    g_enemySpeedCps = g_enemySpeedTargetCps;

    // Инициализация состояния игрока для плавного движения/одновременной стрельбы
    g_playerXf = (float)player.x;
    g_playerMoveDir = 0;
    g_playerMoveHeldUntil = 0.0;
    g_shootPressed = 0;
    g_shootHeldUntil = 0.0;
    g_shootCooldownS = 0.0f;
    
    // Деактивируем все пули
    for (int i = 0; i < MAX_BULLETS; i++) {
        bullets[i].active = 0;
    }
    
    // Деактивируем всех врагов
    for (int i = 0; i < MAX_ENEMIES; i++) {
        enemies[i].active = 0;
    }
    
    // Инициализируем генератор случайных чисел
    srand(time(NULL));
}

// ============================================================
// ФУНКЦИЯ: Спавнить врага
// Описание: Добавляет нового врага в случайной позиции сверху
// ============================================================
// ============================================================
// ФУНКЦИЯ: Спавнить врага
// Описание: Добавляет нового врага; возвращает 1 при успехе
// ============================================================
int spawnEnemy() {
    // Ищем первое неактивное место в массиве враг
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies[i].active) {
            // Генерируем случайную X координату так, чтобы спрайт из ENEMY_WIDTH символов
            // полностью помещался внутри рамки (между 1 и SCREEN_WIDTH-2).
            const int minX = 1;
            const int maxX = (SCREEN_WIDTH - 2) - (ENEMY_WIDTH - 1);
            const int range = maxX - minX + 1;
            if (range <= 0) return 0;
            enemies[i].x = (rand() % range) + minX;
            // Начальная Y координата в верхней части экрана
            enemies[i].y = 1;
            enemies[i].xf = (float)enemies[i].x;
            enemies[i].yf = (float)enemies[i].y;
            // Активируем врага
            enemies[i].active = 1;
            // Случайное начальное направление движения
            enemies[i].direction = (rand() % 2) ? 1 : -1;
            return 1;  // Успешно заспавнили
        }
    }
    return 0; // Места нет — спавн не выполнен
}

// ============================================================
// ФУНКЦИЯ: Выстрелить пулей
// Описание: Создает новую пулю на позиции игрока
// ============================================================
void shootBullet() {
    // Ищем первое неактивное место для пули
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (!bullets[i].active) {
            // Пуля появляется на позиции игрока, на один шаг выше
            bullets[i].x = player.x;
            bullets[i].y = player.y - 1;
            bullets[i].active = 1;
            return;
        }
    }
}

// ============================================================
// ФУНКЦИЯ: Обработка ввода с клавиатуры
// Описание: Обрабатывает нажатия клавиш для управления игроком
// Параметр ch: Код нажатой клавиши
// ============================================================
void handleInput(int ch) {
    // Если нет ввода, выходим
    if (ch == ERR) return;
    
    // Нормализуем: ASCII приводим к нижнему регистру, KEY_* оставляем как есть
    ch = normalizeKey(ch);
    
    // Обрабатываем нажатие клавиши
    const double nowT = monotonicSeconds();
    switch (ch) {
        // Клавиша 'A' или стрелка влево - движение влево
        case 'a':
        case KEY_LEFT:
            g_playerMoveDir = -1;
            g_playerMoveHeldUntil = nowT + MOVE_HOLD_GRACE_S;
            break;
        
        // Клавиша 'D' или стрелка вправо - движение вправо
        case 'd':
        case KEY_RIGHT:
            g_playerMoveDir = 1;
            g_playerMoveHeldUntil = nowT + MOVE_HOLD_GRACE_S;
            break;
        
        // Пробел - выстрел
        case ' ':
            g_shootPressed = 1;
            g_shootHeldUntil = nowT + SHOOT_HOLD_GRACE_S;
            break;
        
        // Клавиша 'Q' - выход из игры
        case 'q':
            gameOver = 1;   // Устанавливаем флаг завершения игры
            break;
    }
}

static void updatePlayer(float dt, double nowT) {
    // Движение: если недавно была A/D, продолжаем двигаться каждый кадр
    if (nowT <= g_playerMoveHeldUntil && g_playerMoveDir != 0) {
        g_playerXf += (float)g_playerMoveDir * PLAYER_SPEED_CPS * dt;
    }

    if (g_playerXf < 1.0f) g_playerXf = 1.0f;
    if (g_playerXf > (float)(SCREEN_WIDTH - 2)) g_playerXf = (float)(SCREEN_WIDTH - 2);
    // Для положительных координат достаточно округления через +0.5.
    player.x = (int)(g_playerXf + 0.5f);

    // Стрельба: одиночный выстрел по нажатию + автострельба при удержании (через кулдаун)
    g_shootCooldownS -= dt;
    if (g_shootCooldownS < 0.0f) g_shootCooldownS = 0.0f;

    const int shootingHeld = (nowT <= g_shootHeldUntil);
    if ((g_shootPressed || shootingHeld) && g_shootCooldownS <= 0.0f) {
        shootBullet();
        g_shootCooldownS = PLAYER_FIRE_COOLDOWN_S;
    }
    g_shootPressed = 0;
}

// ============================================================
// ФУНКЦИЯ: Обновить пули
// Описание: Перемещает активные пули вверх по экрану
// ============================================================
void updateBullets() {
    // Проходим по всем пулям
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (bullets[i].active) {
            // Перемещаем пулю на 1 позицию вверх
            bullets[i].y--;
            
            // Если пуля вышла за верхнюю границу экрана, деактивируем её
            if (bullets[i].y <= 0) {
                bullets[i].active = 0;
            }
        }
    }
}

// ============================================================
// ФУНКЦИЯ: Обновить врагов
// Описание: Управляет спавном и движением врагов
// ============================================================
void updateEnemies(float dt) {
    // Счетчики времени спавна и движения (сбрасываются при новой игре)
    enemySpawnCounter++;
    
    // ========== СПАВН ВРАГОВ ==========
    // Вычисляем частоту спавна враг (зависит от волны)
    // На каждой новой волне враги появляются чаще
    int spawnRate = ENEMY_SPAWN_BASE - (wave * ENEMY_SPAWN_WAVE_STEP);
    // Устанавливаем минимальную частоту спавна
    if (spawnRate < ENEMY_SPAWN_MIN) spawnRate = ENEMY_SPAWN_MIN;
    
    // Спавним врага если пришло время И не превышен лимит враг в волне
    const int enemiesThisWave = wave * ENEMIES_PER_WAVE;

    if (enemySpawnCounter >= spawnRate && totalEnemiesSpawned < enemiesThisWave) {
        // Важно: увеличиваем счетчик спавна ТОЛЬКО если реально заспавнили
        if (spawnEnemy()) {
            totalEnemiesSpawned++;
            enemySpawnCounter = 0;  // Сбрасываем счетчик
        } else {
            // Если места нет, не накручиваем totalEnemiesSpawned и пробуем снова на следующем кадре
            enemySpawnCounter = spawnRate;
        }
    }
    
    // ========== ДВИЖЕНИЕ ВРАГ (ПО ВРЕМЕНИ, ПЛАВНО) ==========
    // Берем "старую" настройку скорости (в кадрах) и переводим в клетки/сек.
    // Затем плавно ведем текущую скорость к целевой (ограничиваем ускорение).
    const float fpsApprox = 1000.0f / (float)FRAME_DELAY_MS;

    int moveDelayFrames = ENEMY_MOVE_BASE - (wave / ENEMY_MOVE_WAVE_DIV);
    if (moveDelayFrames < ENEMY_MOVE_MIN) moveDelayFrames = ENEMY_MOVE_MIN;
    g_enemySpeedTargetCps = fpsApprox / (float)moveDelayFrames;

    const float maxSpeedDelta = ENEMY_SPEED_SLEW_CPS2 * dt;
    const float diff = g_enemySpeedTargetCps - g_enemySpeedCps;
    g_enemySpeedCps += clampf(diff, -maxSpeedDelta, maxSpeedDelta);

    const int minX = 1;
    const int maxX = (SCREEN_WIDTH - 2) - (ENEMY_WIDTH - 1);
    const float delta = g_enemySpeedCps * dt;

    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!enemies[i].active) continue;

        enemies[i].xf += (float)enemies[i].direction * delta;
        enemies[i].yf += delta;

        // Горизонтальные границы: отражаемся и кладем на границу.
        if (enemies[i].xf <= (float)minX) {
            enemies[i].xf = (float)minX;
            enemies[i].direction = 1;
        } else if (enemies[i].xf >= (float)maxX) {
            enemies[i].xf = (float)maxX;
            enemies[i].direction = -1;
        }

        // Переводим дробную позицию в целочисленную для отрисовки/коллизий.
        // Для положительных координат простое округление через +0.5 достаточно.
        enemies[i].x = (int)(enemies[i].xf + 0.5f);
        enemies[i].y = (int)(enemies[i].yf + 0.5f);

        // Нижняя граница
        if (enemies[i].y >= SCREEN_HEIGHT - 1) {
            enemies[i].active = 0;
            totalEnemiesDefeated++;
            player.health--;
            if (player.health <= 0) gameOver = 1;
        }
    }
}

// ============================================================
// ФУНКЦИЯ: Проверить столкновения
// Описание: Проверяет столкновения пуль с врагами и врагов с игроком
// ============================================================
void checkCollisions() {
    // ========== СТОЛКНОВЕНИЕ ПУЛЬ С ВРАГАМИ ==========
    // Проходим по всем пулям
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (bullets[i].active) {
            // Для каждой пули проверяем столкновение со всеми врагами
            for (int j = 0; j < MAX_ENEMIES; j++) {
                if (enemies[j].active) {
                    // Пуля попала в один из символов спрайта врага (ширина ENEMY_WIDTH)
                    if (bullets[i].y == enemies[j].y &&
                        bullets[i].x >= enemies[j].x &&
                        bullets[i].x < enemies[j].x + ENEMY_WIDTH) {
                        bullets[i].active = 0;      // Деактивируем пулю
                        enemies[j].active = 0;      // Деактивируем врага
                        player.score += 10;         // Добавляем 10 очков
                        totalEnemiesDefeated++;      // Враг "убран" (убит)
                        break;                       // Пуля исчезла — дальше этой пулей не бьем
                    }
                }
            }
        }
    }
    
    // ========== СТОЛКНОВЕНИЕ ВРАГА С ИГРОКОМ ==========
    // Проходим по всем врагам
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (enemies[i].active) {
            // Если игрок задел один из символов спрайта врага
            if (enemies[i].y == player.y &&
                player.x >= enemies[i].x &&
                player.x < enemies[i].x + ENEMY_WIDTH) {
                enemies[i].active = 0;      // Деактивируем врага
                totalEnemiesDefeated++;      // Враг "убран" (столкновение)
                player.health--;            // Уменьшаем здоровье игрока
                // Если здоровье закончилось, заканчиваем игру
                if (player.health <= 0) {
                    gameOver = 1;
                }
            }
        }
    }
    
    // ========== ПРОВЕРКА ПЕРЕХОДА НА НОВУЮ ВОЛНУ ==========
    // Переходим на новую волну если:
    // 1. Все враги этой волны спавнились
    // 2. Все враги этой волны побеждены
    const int enemiesThisWave = wave * ENEMIES_PER_WAVE;

    if (totalEnemiesSpawned >= enemiesThisWave && totalEnemiesDefeated >= totalEnemiesSpawned) {
        wave++;                  // Переходим на следующую волну
        totalEnemiesSpawned = 0; // Сбрасываем счетчик спавнены враг
        totalEnemiesDefeated = 0;// Сбрасываем счетчик побежденных враг
        enemySpawnCounter = 0;   // Сбрасываем таймеры, чтобы новая волна стартовала предсказуемо

        // На всякий случай чистим остатки (пули/враги) между волнами
        for (int i = 0; i < MAX_BULLETS; i++) bullets[i].active = 0;
        for (int i = 0; i < MAX_ENEMIES; i++) enemies[i].active = 0;
    }
}

// ============================================================
// ФУНКЦИЯ: Рендер (отрисовка) игры
// Описание: Отрисовывает все объекты и информацию на экране
// ============================================================
void render() {
    // Важно: используем erase(), а не clear().
    // clear() может заставлять терминал делать полный redraw и вызывать мерцание.
    erase();  // Очищаем буфер экрана
    
    // ========== РИСУЕМ РАМКУ ВОКРУГ ЭКРАНА ==========
    // Верхний левый угол
    mvaddch(0, 0, ACS_ULCORNER);
    // Верхняя горизонтальная линия
    for (int i = 1; i < SCREEN_WIDTH - 1; i++) {
        mvaddch(0, i, ACS_HLINE);
    }
    // Верхний правый угол
    mvaddch(0, SCREEN_WIDTH - 1, ACS_URCORNER);
    
    // Левая и правая вертикальные линии
    for (int i = 1; i < SCREEN_HEIGHT - 1; i++) {
        mvaddch(i, 0, ACS_VLINE);
        mvaddch(i, SCREEN_WIDTH - 1, ACS_VLINE);
    }
    
    // Нижний левый угол
    mvaddch(SCREEN_HEIGHT - 1, 0, ACS_LLCORNER);
    // Нижняя горизонтальная линия
    for (int i = 1; i < SCREEN_WIDTH - 1; i++) {
        mvaddch(SCREEN_HEIGHT - 1, i, ACS_HLINE);
    }
    // Нижний правый угол
    mvaddch(SCREEN_HEIGHT - 1, SCREEN_WIDTH - 1, ACS_LRCORNER);
    
    // ========== РИСУЕМ ИГРОКА ==========
    // Символ '^' представляет кораблик игрока
    mvaddch(player.y, player.x, '^');
    
    // ========== РИСУЕМ ВРАГОВ ==========
    // Враг — спрайт из ENEMY_WIDTH символов
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (enemies[i].active) {
            // Проверяем что враг видим на экране
            if (enemies[i].y > 0 && enemies[i].y < SCREEN_HEIGHT) {
                mvaddnstr(enemies[i].y, enemies[i].x, ENEMY_SPRITE, ENEMY_WIDTH);
            }
        }
    }
    
    // ========== РИСУЕМ ПУЛИ ==========
    // Символ '|' представляет пулю
    for (int i = 0; i < MAX_BULLETS; i++) {
        if (bullets[i].active) {
            // Проверяем что пуля видима на экране
            if (bullets[i].y > 0 && bullets[i].y < SCREEN_HEIGHT) {
                mvaddch(bullets[i].y, bullets[i].x, '|');
            }
        }
    }
    
    // Обновляем фактический размер терминала (для вывода подсказок вне рамки)
    getmaxyx(stdscr, g_termMaxY, g_termMaxX);

    // ========== ВЫВОДИМ ИНФОРМАЦИЮ НА ЭКРАН ==========
    // Название игры
    mvprintw(0, 2, "GALAGA");
    // Текущие очки
    mvprintw(1, 2, " Score: %d ", player.score);
    // Текущее здоровье (жизни)
    mvprintw(2, 2, " Health: %d ", player.health);
    // Текущая волна
    mvprintw(3, 2, " Wave: %d ", wave);
    
    // Подсказка управления: НЕ печатаем на линии рамки (иначе пробелы "протирают" рамку).
    // Если терминал выше игрового поля, печатаем строку ниже рамки.
    if (g_termMaxY > SCREEN_HEIGHT) {
        mvprintw(SCREEN_HEIGHT, 2, "A/D: Move | Space: Shoot | Q: Quit");
    }
    
    // Обновляем экран без мерцания: копим изменения и применяем одним doupdate().
    // Используем wnoutrefresh(stdscr), т.к. noutrefresh() может быть недоступен в некоторых сборках curses.
    wnoutrefresh(stdscr);
    doupdate();
}

// ============================================================
// ФУНКЦИЯ: Главный игровой цикл
// Описание: Основной цикл игры, который повторяется каждый кадр
// ============================================================
void gameLoop() {
    // Включаем неблокирующий режим чтения (getch() не ждет ввода)
    nodelay(stdscr, TRUE);
    // Неблокирующее чтение: getch() сразу возвращает ERR, если ввода нет
    // (а FPS регулируем через napms(FRAME_DELAY_MS)).
    wtimeout(stdscr, 0);
    
    // Основной игровой цикл - продолжается пока игра не завершена
    double prevT = monotonicSeconds();
    while (!gameOver) {
        const double nowT = monotonicSeconds();
        float dt = (float)(nowT - prevT);
        prevT = nowT;
        // Защита от "скачков" времени (например, если окно терминала подвисло)
        dt = clampf(dt, 0.0f, 0.05f);

        // Считываем все доступные нажатия в текущем кадре.
        // Это позволяет обработать движение и выстрел «одновременно» (в одном кадре),
        // если пользователь нажал/удерживает несколько клавиш.
        // Важно: если держать клавишу, терминал генерирует автоповтор.
        // Без лимита можно "утонуть" в вводе и перестать обновлять игру.
        for (int i = 0; i < MAX_INPUTS_PER_FRAME; i++) {
            int ch = getch();
            if (ch == ERR) break;
            handleInput(ch);
        }

        // Применяем ввод к игроку (движение + возможный выстрел)
        updatePlayer(dt, nowT);
        
        // Обновляем состояние игры каждый кадр
        updateBullets();   // Перемещаем пули
        updateEnemies(dt); // Управляем врагами
        checkCollisions(); // Проверяем столкновения
        
        // Отрисовываем игру на экране
        render();

        // Держим стабильную скорость обновления
        napms(FRAME_DELAY_MS);
    }
}

// ============================================================
// ФУНКЦИЯ: Экран завершения игры
// Описание: Показывает финальную статистику и спрашивает перезапуск
// Возврат: 1 - перезапуск, 0 - выход
// ============================================================
int endGame() {
    // Аналогично render(): erase() уменьшает мерцание при выводе экрана завершения.
    erase();
    
    // Выводим сообщение об окончании игры по центру
    mvprintw(SCREEN_HEIGHT / 2 - 3, SCREEN_WIDTH / 2 - 15, "GAME OVER!");
    // Выводим финальный счет
    mvprintw(SCREEN_HEIGHT / 2 - 1, SCREEN_WIDTH / 2 - 15, "Final Score: %d", player.score);
    // Выводим достигнутую волну
    mvprintw(SCREEN_HEIGHT / 2, SCREEN_WIDTH / 2 - 15, "Wave Reached: %d", wave);
    // Выводим подсказку для действия
    mvprintw(SCREEN_HEIGHT / 2 + 2, SCREEN_WIDTH / 2 - 15, "Press R to restart");
    mvprintw(SCREEN_HEIGHT / 2 + 3, SCREEN_WIDTH / 2 - 15, "Press Q or ESC to quit");
    // Обновляем экран без мерцания
    wnoutrefresh(stdscr);
    doupdate();
    
    // Ждем корректный ввод пользователя
    int ch;
    while (1) {
        ch = getch();
        if (ch == 'r' || ch == 'R') return 1; // Перезапуск
        if (ch == 'q' || ch == 'Q' || ch == 27) return 0; // Выход
    }
}

// ============================================================
// ГЛАВНАЯ ФУНКЦИЯ
// Описание: Точка входа программы, инициализирует ncurses и циклический рестарт игры
// ============================================================
int main() {
    // ========== ИНИЦИАЛИЗАЦИЯ NCURSES ==========
    initscr();              // Инициализируем ncurses
    raw();                  // Включаем режим raw (посимвольный ввод)
    noecho();               // Отключаем эхо (символы не выводятся)
    keypad(stdscr, TRUE);   // Включаем поддержку специальных клавиш (стрелки, F1 и т.д.)
    curs_set(0);            // Скрываем курсор
    
    // ========== ПРОВЕРКА РАЗМЕРА ТЕРМИНАЛА ==========
    int maxX, maxY;
    getmaxyx(stdscr, maxY, maxX);  // Получаем размеры терминала
    
    // Проверяем минимальные требования (по настройкам экрана)
    if (maxX < SCREEN_WIDTH || maxY < SCREEN_HEIGHT) {
        endwin();           // Завершаем работу ncurses
        // Выводим сообщение об ошибке
        printf("Требуется размер терминала минимум %dx%d\n", SCREEN_WIDTH, SCREEN_HEIGHT);
        return 1;           // Возвращаем код ошибки
    }
    
    // ========== ЦИКЛ ИГРЫ С ПЕРЕЗАПУСКОМ ==========
    while (1) {
        initGame();      // Инициализируем игру перед запуском
        gameLoop();      // Запускаем основной игровой цикл
        // Если игрок выбрал выход, прерываем цикл
        if (!endGame()) {
            break;
        }
    }
    
    // ========== ЗАВЕРШЕНИЕ ПРОГРАММЫ ==========
    endwin();        // Завершаем работу ncurses
    return 0;        // Возвращаем успешный код выхода
}
