// A4: successful, failed, absent, ambiguous and injector-origin observations.
#include "test_support.hpp"
#include "providers/discovery/upscaler_report.hpp"
using namespace na::discovery;
int main() { return test::run({
    {"requests and replacements are not successful native evaluations", [] {
        UpscalerSnapshot s{}; s[1].observer_available = true; s[1].game_requests = 5;
        auto r = DescribeUpscalers(s);
        CHECK(r.game_routes == 0 && r.rows[1].detection == Detection::Requested);
        s[0].injector_evaluates = 5;
        r = DescribeUpscalers(s);
        CHECK(r.rows[0].detection == Detection::InjectorOnly && r.game_routes == 0);
        s[1].game_creates = 1;
        r = DescribeUpscalers(s);
        CHECK(r.game_routes == 2 && r.rows[1].detection == Detection::Created);
        CHECK(r.rows[1].reason.find("native evaluation is unverified") != std::string::npos);
    }},
    {"absence of observation is not absence of integration", [] {
        UpscalerSnapshot s{};
        auto r = DescribeUpscalers(s);
        CHECK(r.game_routes == 0);
        CHECK(std::string(r.rows[1].badge) == "NOT MONITORED");
        s[0].observer_available = true;
        r = DescribeUpscalers(s);
        CHECK(std::string(r.rows[0].badge) == "NOT OBSERVED");
        CHECK(r.rows[0].reason.find("does not prove absence") != std::string::npos);
    }},
    {"library and failed calls cannot establish a route", [] {
        UpscalerSnapshot s{}; s[0].module_seen = true;
        auto r = DescribeUpscalers(s);
        CHECK(r.game_routes == 0 && r.rows[0].detection == Detection::ModuleOnly);
        s[0].failed_game_calls = 1;
        r = DescribeUpscalers(s);
        CHECK(r.game_routes == 0 && r.rows[0].detection == Detection::FailedCalls);
    }},
    {"injector and unknown sources never masquerade as native support", [] {
        UpscalerSnapshot s{}; s[0].injector_evaluates = 100;
        auto r = DescribeUpscalers(s);
        CHECK(r.game_routes == 0 && r.rows[0].detection == Detection::InjectorOnly);
        CHECK(r.rows[0].reason.find("Historical evidence") == std::string::npos);
        CHECK(r.rows[0].reason.find("observer is not currently available") == std::string::npos);
        s[0].unknown_creates = 1;
        r = DescribeUpscalers(s);
        CHECK(r.game_routes == 0 && r.rows[0].detection == Detection::SourceUnknown);
        CHECK(r.summary.find("source unconfirmed") != std::string::npos);
    }},
    {"creation and evaluation are different evidence", [] {
        UpscalerSnapshot s{}; s[0].observer_available = true; s[0].game_creates = 1;
        auto r = DescribeUpscalers(s);
        CHECK(r.game_routes == 1 && r.rows[0].detection == Detection::Created);
        s[0].game_evaluates = 5; s[0].failed_game_calls = 1;
        r = DescribeUpscalers(s);
        CHECK(r.rows[0].detection == Detection::Evaluated);
        CHECK(r.rows[0].reason.find("not qualified") != std::string::npos);
        CHECK(r.rows[0].reason.find("Failed calls") != std::string::npos);
    }},
    {"multiple integrations remain visible without choosing one", [] {
        UpscalerSnapshot s{}; s[0].game_creates = 1; s[1].game_evaluates = 2; s[2].game_evaluates = 3;
        const auto r = DescribeUpscalers(s);
        CHECK(r.game_routes == 7);
        CHECK(r.summary.find("DLSS / FSR / XeSS") != std::string::npos);
        CHECK(r.rows[2].reason.find("Historical evidence") != std::string::npos);
    }},
    {"new snapshot clears prior process evidence", [] {
        UpscalerSnapshot s{}; s[1].game_evaluates = UINT64_MAX;
        CHECK(DescribeUpscalers(s).game_routes == 2);
        s = {};
        CHECK(DescribeUpscalers(s).game_routes == 0);
    }},
}); }
