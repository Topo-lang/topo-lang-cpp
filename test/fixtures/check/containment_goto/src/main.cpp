namespace app {

int parse(int input) {
    if (input < 0) goto error;
    return 0;
error:
    return -1;
}

} // namespace app
