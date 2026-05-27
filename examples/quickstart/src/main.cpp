#include <cstdio>

namespace orders {
struct Order {
    int order_id, cust_id;
    double amount;
    int items;
    Order(int id, int cid) : order_id(id), cust_id(cid), amount(99.95), items(3) {}
    int id() const { return order_id; }
    int customer_id() const { return cust_id; }
    double total() const { return amount; }
    int item_count() const { return items; }
    bool is_valid() const { return items > 0 && amount > 0.0; }
};
struct Invoice {
    int inv_number;
    double total;
    bool finalized;
    int number() const { return inv_number; }
    double grand_total() const { return total; }
    bool is_finalized() const { return finalized; }
};
Invoice process_order(const Order& order);
} // namespace orders

int main() {
    orders::Order order(42, 7);
    std::printf("Processing order #%d for customer #%d...\n", order.id(), order.customer_id());

    orders::Invoice invoice = orders::process_order(order);

    std::printf("Done: Invoice #%d, grand total $%.2f\n", invoice.number(), invoice.grand_total());
    return 0;
}
