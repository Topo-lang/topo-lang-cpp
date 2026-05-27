// Even though `compute` would be impure under purity checks, the Topo.toml
// has `[purity].mode = "off"` so the check must emit a Note diagnostic and
// return 0 regardless of the host code.

static int impurity = 0;

void compute() {
    impurity = impurity + 1;  // would violate — but mode=off
}

void render() {
    impurity = impurity * 2;  // would violate — but mode=off
}
