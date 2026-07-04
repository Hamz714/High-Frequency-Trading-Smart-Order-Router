#include "SmartOrderRouter.h"

SmartOrderRouter::SmartOrderRouter(const RouterConfig& cfg):
    config(cfg), dp_engine(cfg), running(false) {}

SmartOrderRouter::~SmartOrderRouter() {
    stop();
}

void SmartOrderRouter::add_venue(Venue* venue) {
    VenueID id = venue->get_id(); 
    
    venues[id] = venue;

    venue_md_queues[id] = std::make_unique<SPSCQueue<BookDelta, QUEUE_SIZE>>();
    venue_fill_queues[id] = std::make_unique<SPSCQueue<FillEvent, QUEUE_SIZE>>();

    mirror_books[id] = LimitOrderBook();

    venue->set_sor_queues(venue_md_queues[id].get(), venue_fill_queues[id].get());
}

void SmartOrderRouter::start() {
    if (running.exchange(true)) return; 

    md_thread = std::thread(&SmartOrderRouter::market_data_loop, this);
    fill_thread = std::thread(&SmartOrderRouter::fill_loop, this);
    order_thread = std::thread(&SmartOrderRouter::client_order_loop, this);
}

void SmartOrderRouter::stop() {
    if (!running.exchange(false)) return; 

    if (md_thread.joinable()) md_thread.join();
    if (fill_thread.joinable()) fill_thread.join();
    if (order_thread.joinable()) order_thread.join();
}

void SmartOrderRouter::submit_order(const OrderRequest& client_request) {
    client_inbox.push(client_request);
}