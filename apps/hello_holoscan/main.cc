// Minimal Holoscan application: TxOp emits a counter, RxOp logs it.
#include <holoscan/holoscan.hpp>

#include <cstdio>

namespace hello_holoscan {

class TxOp : public holoscan::Operator {
 public:
  HOLOSCAN_OPERATOR_FORWARD_ARGS(TxOp)
  TxOp() = default;

  void setup(holoscan::OperatorSpec& spec) override { spec.output<int>("out"); }

  void compute(holoscan::InputContext&, holoscan::OutputContext& op_output,
               holoscan::ExecutionContext&) override {
    op_output.emit(++count_, "out");
  }

 private:
  int count_ = 0;
};

class RxOp : public holoscan::Operator {
 public:
  HOLOSCAN_OPERATOR_FORWARD_ARGS(RxOp)
  RxOp() = default;

  void setup(holoscan::OperatorSpec& spec) override { spec.input<int>("in"); }

  void compute(holoscan::InputContext& op_input, holoscan::OutputContext&,
               holoscan::ExecutionContext&) override {
    auto value = op_input.receive<int>("in");
    if (value) {
      HOLOSCAN_LOG_INFO("rx received {}", *value);
      ++received_;
    }
  }

  int received() const { return received_; }

 private:
  int received_ = 0;
};

class App : public holoscan::Application {
 public:
  void compose() override {
    auto tx = make_operator<TxOp>("tx", make_condition<holoscan::CountCondition>(10));
    rx_ = make_operator<RxOp>("rx");
    add_flow(tx, rx_, {{"out", "in"}});
  }

  int received() const { return rx_ ? rx_->received() : 0; }

 private:
  std::shared_ptr<RxOp> rx_;
};

}  // namespace hello_holoscan

int main() {
  auto app = holoscan::make_application<hello_holoscan::App>();
  app->run();
  std::printf("hello_holoscan: rx received %d messages\n", app->received());
  return app->received() == 10 ? 0 : 1;
}
