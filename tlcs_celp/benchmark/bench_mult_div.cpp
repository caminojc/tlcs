#include <stdio.h>
#include <stdlib.h>
#include<iostream>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

using namespace std;

#ifdef _WIN32
class timing_c{
public:
    timing_c()
    {
        QueryPerformanceFrequency(&m_frequency);
    }

    void tic()
    {
        QueryPerformanceCounter(&m_startT);  
    }

    double toc()
    {
        LARGE_INTEGER t2;     
        QueryPerformanceCounter(&t2);   
        m_totElapsedTime += (t2.QuadPart - m_startT.QuadPart) * 1000.0 / m_frequency.QuadPart;
        return m_totElapsedTime;
    }

private:
    LARGE_INTEGER m_frequency;        // ticks per second
    double m_totElapsedTime{0.0};
    LARGE_INTEGER m_startT{0};    
};
#else
class timing_c{
public:
    timing_c()
    {
    }

    void tic()
    {
        m_startT = clock();
    }

    double toc()
    {
        clock_t t2 = clock();
        m_totElapsedTime += (t2 - m_startT);
        return (m_totElapsedTime * 1000.0) / CLOCKS_PER_SEC;
    }

private:
    clock_t m_totElapsedTime{0};
    clock_t m_startT{0};    
};
#endif

int main( int argc, char** argv )
{
    printf("Benchmarking of multiplications and divisions !! \n");

    #define N 1000000
    float *a_vec = (float*)malloc(N*sizeof(float));
    float *b_vec = (float*)malloc(N*sizeof(float));    

    {    
        timing_c timer;
        timer.tic();
        for(int64_t n = 0; n < N; n++){
            a_vec[n] = (float)(rand());
            b_vec[n] = (float)(rand() + 1);
        }

        double totElapsedTime = timer.toc();
        printf("Generating 2x%d rand took %f ms \n", N, totElapsedTime);
    }

    {    
        timing_c timer;
        timer.tic();
        double tot = 0.0;
        for(int64_t n = 0; n < N; n++){
            tot += a_vec[n] * b_vec[n];
        }

        double totElapsedTime = timer.toc();
        printf("Multiply accumulate 2x%d vector took %f ms result = %f \n", N, totElapsedTime, tot);
    }

    {    
        timing_c timer;
        timer.tic();
        double tot = 0.0;
        for(int64_t n = 0; n < N; n++){
            tot += a_vec[n] / b_vec[n];
        }

        double totElapsedTime = timer.toc();
        printf("Divide accumulate 2x%d vector took %f ms result = %f \n", N, totElapsedTime, tot);
    }

    free(a_vec);
    free(b_vec);    
}