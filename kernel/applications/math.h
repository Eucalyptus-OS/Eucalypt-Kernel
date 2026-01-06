#ifndef MATH_H
#define MATH_H

static inline double fabs(double x) {
    return x < 0.0 ? -x : x;
}

static inline float fabsf(float x) {
    return x < 0.0f ? -x : x;
}

static inline double pow(double base, int exp) {
    double result = 1.0;
    int positive_exp = exp < 0 ? -exp : exp;
    
    for (int i = 0; i < positive_exp; i++) {
        result *= base;
    }
    
    return exp < 0 ? 1.0 / result : result;
}

static inline double sqrt(double x) {
    if (x < 0.0) return 0.0;
    if (x == 0.0) return 0.0;
    
    double guess = x;
    double epsilon = 0.00001;
    
    while (fabs(guess * guess - x) > epsilon) {
        guess = (guess + x / guess) / 2.0;
    }
    
    return guess;
}

static inline double floor(double x) {
    return (double)((long)x - (x < 0.0 && x != (long)x));
}

static inline double ceil(double x) {
    return (double)((long)x + (x > 0.0 && x != (long)x));
}

static inline double round(double x) {
    return x >= 0.0 ? floor(x + 0.5) : ceil(x - 0.5);
}

static inline int min_int(int a, int b) {
    return a < b ? a : b;
}

static inline int max_int(int a, int b) {
    return a > b ? a : b;
}

static inline double min_double(double a, double b) {
    return a < b ? a : b;
}

static inline double max_double(double a, double b) {
    return a > b ? a : b;
}

#endif