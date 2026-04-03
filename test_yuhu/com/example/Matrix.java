package com.example;

public class Matrix {
    // 矩阵乘法方法 - 这个方法会被 Yuhu 编译
    public static void multiply(int[][] a, int[][] b, int[][] c, int n) {
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < n; j++) {
                c[i][j] = 0;
                for (int k = 0; k < n; k++) {
                    c[i][j] += a[i][k] * b[k][j];
                }
            }
        }
    }
    
    // Main 方法用于测试
    public static void main(String[] args) {
        int n = 100;
        int[][] a = new int[n][n];
        int[][] b = new int[n][n];
        int[][] c = new int[n][n];
        
        // 初始化矩阵
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < n; j++) {
                a[i][j] = i + j;
                b[i][j] = i - j;
            }
        }
        
        // 多次调用 multiply 方法，使其成为热点方法
        System.out.println("开始测试矩阵乘法...");
        long start = System.currentTimeMillis();
        
        for (int i = 0; i < 1000; i++) {
            System.out.println("loop " + i + " c[0][0] " + c[0][0]);
            multiply(a, b, c, n);
        }
        
        long end = System.currentTimeMillis();
        System.out.println("完成！耗时: " + (end - start) + " ms");
        System.out.println("结果示例: c[0][0] = " + c[0][0]);
    }
}



