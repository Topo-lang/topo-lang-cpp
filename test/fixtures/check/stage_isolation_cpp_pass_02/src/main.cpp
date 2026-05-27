// Both `taskA` and `taskB` are in stage<1>. Same-stage calls are allowed.

void taskB() {
    // stage 1 work
}

void taskA() {
    taskB();  // same-stage call — OK
}

void run() {
    taskA();
    taskB();
}
