#include <cstdio>

namespace orders {

// --- Order ---

struct Order {
    int order_id;
    int cust_id;
    double amount;
    int items;

    Order(int id, int customer_id) : order_id(id), cust_id(customer_id), amount(99.95), items(3) {}

    int id() const { return order_id; }
    int customer_id() const { return cust_id; }
    double total() const { return amount; }
    int item_count() const { return items; }
    bool is_valid() const { return items > 0 && amount > 0.0; }
};

// --- PaymentResult ---

struct PaymentResult {
    bool is_approved;
    int txn_id;
    double charged;

    bool approved() const { return is_approved; }
    int transaction_id() const { return txn_id; }
    double charged_amount() const { return charged; }
};

// --- ShippingQuote ---

struct ShippingQuote {
    double ship_cost;
    int days;
    int carrier;

    double cost() const { return ship_cost; }
    int estimated_days() const { return days; }
    int carrier_id() const { return carrier; }
};

// --- Invoice ---

struct Invoice {
    int inv_number;
    double total;
    bool finalized;

    Invoice() : inv_number(0), total(0.0), finalized(false) {}
    Invoice(int order_id, int transaction_id, double t)
        : inv_number(order_id * 1000 + transaction_id), total(t), finalized(true) {}

    int number() const { return inv_number; }
    double grand_total() const { return total; }
    bool is_finalized() const { return finalized; }
};

// --- Private helpers ---

bool check_inventory(int /*item_id*/, int /*quantity*/) {
    return true; // simulated: always in stock
}

bool verify_address(int /*customer_id*/) {
    return true; // simulated: address always valid
}

double apply_discount(double total, int customer_id) {
    // VIP customers (id < 100) get 10% off
    if (customer_id < 100) {
        return total * 0.9;
    }
    return total;
}

// --- Internal ---

void dump_order_state(const Order& order) {
    std::printf("[DEBUG] Order #%d: customer=%d, total=%.2f, items=%d, valid=%s\n",
                order.id(),
                order.customer_id(),
                order.total(),
                order.item_count(),
                order.is_valid() ? "yes" : "no");
}

// --- Protected ---

bool validate_order(const Order& order) {
    if (!order.is_valid()) return false;
    if (!check_inventory(order.id(), order.item_count())) return false;
    if (!verify_address(order.customer_id())) return false;
    return true;
}

PaymentResult charge_payment(const Order& order) {
    double charged = apply_discount(order.total(), order.customer_id());
    return PaymentResult{true, order.id() * 10 + 1, charged};
}

ShippingQuote calculate_shipping(const Order& order) {
    double cost = 5.0 + order.item_count() * 1.5;
    int days = order.item_count() <= 5 ? 3 : 7;
    return ShippingQuote{cost, days, 42};
}

Invoice create_invoice(const Order& order, const PaymentResult& payment, const ShippingQuote& shipping) {
    double grand_total = payment.charged_amount() + shipping.cost();
    return Invoice(order.id(), payment.transaction_id(), grand_total);
}

// --- Private ---

void send_confirmation(const Invoice& invoice) {
    std::printf("Confirmation: Invoice #%d, total $%.2f\n", invoice.number(), invoice.grand_total());
}

void update_analytics(const Order& order, const Invoice& invoice) {
    std::printf("Analytics: order=%d, invoice=%d, total=$%.2f\n", order.id(), invoice.number(), invoice.grand_total());
}

// --- Public ---

Invoice process_order(const Order& order) {
    validate_order(order);

    PaymentResult payment = charge_payment(order);
    ShippingQuote shipping = calculate_shipping(order);

    Invoice invoice = create_invoice(order, payment, shipping);

    send_confirmation(invoice);
    update_analytics(order, invoice);

    return invoice;
}

} // namespace orders
