<?php
if ($_SERVER['REQUEST_URI'] === '/first') {
    Beacon::setOpt(Beacon::OPT_ON_REGISTER, function ($ctx) { echo 'leaked'; });
    Beacon::setOpt(Beacon::OPT_LB_STRATEGY, Beacon::LB_WEIGHTED);
    echo 'registered';
} else {
    echo beacon_test_callback_invoke(Beacon::OPT_ON_REGISTER, null);
}
