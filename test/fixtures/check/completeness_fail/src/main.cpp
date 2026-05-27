namespace app {

int processOrder(int orderId) {
    return orderId * 2;
}

// This function is not declared in .topo — should cause a completeness error
int undeclaredHelper(int x) {
    return x + 1;
}

} // namespace app
