vec3 lcd_filter7(
    float sample_0, float sample_1, float sample_2, float sample_3,
    float sample_4, float sample_5, float sample_6, bool forward_order)
{
    float filter_edge = 0.03125;
    float filter_side = 0.30078125;
    float filter_center = 0.3359375;
    float first_coverage =
        sample_0 * filter_edge +
        sample_1 * filter_side +
        sample_2 * filter_center +
        sample_3 * filter_side +
        sample_4 * filter_edge;
    float center_coverage =
        sample_1 * filter_edge +
        sample_2 * filter_side +
        sample_3 * filter_center +
        sample_4 * filter_side +
        sample_5 * filter_edge;
    float last_coverage =
        sample_2 * filter_edge +
        sample_3 * filter_side +
        sample_4 * filter_center +
        sample_5 * filter_side +
        sample_6 * filter_edge;

    return forward_order
        ? vec3(first_coverage, center_coverage, last_coverage)
        : vec3(last_coverage, center_coverage, first_coverage);
}
