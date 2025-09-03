// test_vector_analysis.cpp
#include <cstdlib>

// 测试用例1: 标准vector-like结构
struct MyVector {
    int* data;        // 数组头指针
    size_t size;      // 有效长度
    size_t capacity;  // 容量
};

// 测试用例2: 另一种vector-like结构
class SimpleVector {
private:
    char* buffer;
    int count;
    int max_count;
public:
    void push_back(char c);
    void resize(int new_size);
};

// 测试用例3: 非vector结构(缺少容量字段)
struct SimpleArray {
    double* elements;
    int length;
};

// 测试用例4: 非vector结构(没有指针字段)
struct Counter {
    int current;
    int maximum;
    int minimum;
};

// 支持vector模式的函数
void vector_push_back(MyVector* vec, int value) {
    if (vec->size >= vec->capacity) {
        // 需要扩容
        vec->capacity = vec->capacity * 2;
        vec->data = (int*)realloc(vec->data, vec->capacity * sizeof(int));
    }
    vec->data[vec->size] = value;
    vec->size++;
}

// 支持vector模式的函数
void vector_reserve(MyVector* vec, size_t new_capacity) {
    if (new_capacity > vec->capacity) {
        vec->data = (int*)realloc(vec->data, new_capacity * sizeof(int));
        vec->capacity = new_capacity;
    }
}

// 反对vector模式的函数(违反容量约束)
void bad_vector_operation(MyVector* vec, int value) {
    // 直接修改size而不检查capacity
    vec->data[vec->size] = value;
    vec->size++;  // 可能越界
}

// 无关函数
int calculate_sum(int a, int b) {
    return a + b;
}

// SimpleVector的实现
void SimpleVector::push_back(char c) {
    if (count >= max_count) {
        max_count = max_count * 2;
        buffer = (char*)realloc(buffer, max_count);
    }
    buffer[count] = c;
    count++;
}

void SimpleVector::resize(int new_size) {
    if (new_size > max_count) {
        max_count = new_size;
        buffer = (char*)realloc(buffer, max_count);
    }
    count = new_size;
}

// 测试主函数
int main() {
    MyVector vec = {nullptr, 0, 0};
    vector_push_back(&vec, 42);
    vector_reserve(&vec, 100);
    
    SimpleVector svec;
    svec.push_back('a');
    svec.resize(10);
    
    return 0;
}
