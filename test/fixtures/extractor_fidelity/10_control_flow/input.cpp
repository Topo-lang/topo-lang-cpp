// Fixture 10: if / while / for control-flow statements
// Focus: exercises IfStmt, WhileStmt, ForStmt conversion.
// Covers the common combination that most real host code hits.

int classify(int n) {
    int sum = 0;
    if (n < 0) {
        return -1;
    }
    int i = 0;
    while (i < n) {
        sum = sum + i;
        i = i + 1;
    }
    for (int j = 0; j < 3; j = j + 1) {
        sum = sum + j;
    }
    return sum;
}
