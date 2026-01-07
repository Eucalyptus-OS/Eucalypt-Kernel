#include "sys.h"
#include "stdio.h"
#include "string.h"

#define PADDLE_WIDTH 100
#define PADDLE_HEIGHT 20
#define BALL_SIZE 15
#define BRICK_WIDTH 80
#define BRICK_HEIGHT 25
#define BRICKS_PER_ROW 10
#define BRICK_ROWS 5
#define PADDLE_SPEED 8

typedef struct {
    int x, y;
    int vx, vy;
} Ball;

typedef struct {
    int x, y;
    int active;
} Brick;

int main() {
    fb_info_t fb;
    get_fb_info(&fb);
    int paddle_x = fb.width / 2 - PADDLE_WIDTH / 2;
    int paddle_y = fb.height - 50;
    
    Ball ball = {
        .x = fb.width / 2,
        .y = fb.height / 2,
        .vx = 4,
        .vy = -4
    };
    
    Brick bricks[BRICK_ROWS * BRICKS_PER_ROW];
    int brick_count = BRICK_ROWS * BRICKS_PER_ROW;
    
    for (int row = 0; row < BRICK_ROWS; row++) {
        for (int col = 0; col < BRICKS_PER_ROW; col++) {
            int idx = row * BRICKS_PER_ROW + col;
            bricks[idx].x = col * (BRICK_WIDTH + 5) + 50;
            bricks[idx].y = row * (BRICK_HEIGHT + 5) + 50;
            bricks[idx].active = 1;
        }
    }
    
    int score = 0;
    int lives = 3;
    int active_bricks = brick_count;
    
    printf("Starting game! Score: %d Lives: %d\n", score, lives);
    printf("Press any key to start...\n");
    
    while (get_key() == 0) {
        sleep(16);
    }
    
    for (int frame = 0; frame < 10000 && lives > 0 && active_bricks > 0; frame++) {
        uint32_t key = get_key();
        
        if (key != 0) {
            printf("KEY DETECTED: %u (0x%x)\n", key, key);
        }
        
        if (key == 0xFFFFFFEB) {
            paddle_x -= PADDLE_SPEED;
            printf("MOVED LEFT\n");
        }
        else if (key == 0xFFFFFFE9) {
            paddle_x += PADDLE_SPEED;
            printf("MOVED RIGHT\n");
        }
        else if (key == 'a' || key == 'A') {
            paddle_x -= PADDLE_SPEED;
            printf("MOVED LEFT (A)\n");
        }
        else if (key == 'd' || key == 'D') {
            paddle_x += PADDLE_SPEED;
            printf("MOVED RIGHT (D)\n");
        }
        else if (key == 27) {
            printf("ESC pressed, quitting\n");
            break;
        }
        
        if (paddle_x < 0) paddle_x = 0;
        if (paddle_x > fb.width - PADDLE_WIDTH) paddle_x = fb.width - PADDLE_WIDTH;
        
        clear_screen(0x001020);
        
        ball.x += ball.vx;
        ball.y += ball.vy;
        
        if (ball.x <= 0) {
            ball.vx = -ball.vx;
            ball.x = 0;
        }
        if (ball.x >= fb.width - BALL_SIZE) {
            ball.vx = -ball.vx;
            ball.x = fb.width - BALL_SIZE;
        }
        
        if (ball.y <= 0) {
            ball.vy = -ball.vy;
            ball.y = 0;
        }
        
        if (ball.y + BALL_SIZE >= paddle_y &&
            ball.y + BALL_SIZE <= paddle_y + PADDLE_HEIGHT &&
            ball.x + BALL_SIZE >= paddle_x &&
            ball.x <= paddle_x + PADDLE_WIDTH) {
            ball.vy = -ball.vy;
            ball.y = paddle_y - BALL_SIZE;
            
            int paddle_center = paddle_x + PADDLE_WIDTH / 2;
            int ball_center = ball.x + BALL_SIZE / 2;
            int offset = ball_center - paddle_center;
            ball.vx = offset / 8;
        }
        
        for (int i = 0; i < brick_count; i++) {
            if (!bricks[i].active) continue;
            
            if (ball.x + BALL_SIZE >= bricks[i].x &&
                ball.x <= bricks[i].x + BRICK_WIDTH &&
                ball.y + BALL_SIZE >= bricks[i].y &&
                ball.y <= bricks[i].y + BRICK_HEIGHT) {
                
                bricks[i].active = 0;
                ball.vy = -ball.vy;
                score += 10;
                active_bricks--;
                
                if (score % 100 == 0) {
                    printf("Score: %d\n", score);
                }
            }
        }
        
        if (ball.y >= fb.height) {
            lives--;
            ball.x = fb.width / 2;
            ball.y = fb.height / 2;
            ball.vx = 4;
            ball.vy = -4;
            printf("Lost a life! Lives: %d\n", lives);
            sleep(500);
        }
        
        for (int i = 0; i < brick_count; i++) {
            if (bricks[i].active) {
                int row = i / BRICKS_PER_ROW;
                uint32_t colors[] = {0xFF0000, 0xFF8800, 0xFFFF00, 0x00FF00, 0x0088FF};
                fill_rect(bricks[i].x, bricks[i].y, BRICK_WIDTH, BRICK_HEIGHT, colors[row]);
            }
        }
        
        fill_rect(paddle_x, paddle_y, PADDLE_WIDTH, PADDLE_HEIGHT, 0x00FF00);
        
        fill_rect(ball.x, ball.y, BALL_SIZE, BALL_SIZE, 0xFFFFFF);
        
        int score_width = (score * 2 < fb.width) ? score * 2 : fb.width;
        fill_rect(0, 0, score_width, 5, 0xFFFF00);
        
        for (int i = 0; i < lives; i++) {
            fill_rect(fb.width - 40 - i * 35, 10, 25, 25, 0xFF0000);
        }
        
        sleep(5);
    }
    
    clear_screen(0x000000);
    printf("\nGAME OVER\n");
    
    if (active_bricks == 0) {
        printf("*** YOU WIN! ***\n");
    }
    
    printf("Final Score: %d\n", score);
    printf("Lives Remaining: %d\n", lives);
    printf("Bricks Remaining: %d\n", active_bricks);
    
    sleep(3000);
    
    return 0;
}