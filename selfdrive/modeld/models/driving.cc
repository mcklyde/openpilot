
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <eigen3/Eigen/Dense>

#include "common/timing.h"
#include "common/params.h"
#include "driving.h"

constexpr int MIN_VALID_LEN = 10.0;
constexpr int TRAJECTORY_TIME = 10.0;
constexpr float TRAJECTORY_DISTANCE = 192.0;
constexpr int PLAN_IDX = 0;
constexpr int LL_IDX = PLAN_IDX + PLAN_MHP_N*PLAN_MHP_GROUP_SIZE;
constexpr int LL_PROB_IDX = LL_IDX + 4*2*2*33;
constexpr int RE_IDX = LL_PROB_IDX + 4;
constexpr int LEAD_IDX = RE_IDX + 2*2*2*33;
constexpr int LEAD_PROB_IDX = LEAD_IDX + LEAD_MHP_N*(LEAD_MHP_GROUP_SIZE);
constexpr int DESIRE_STATE_IDX = LEAD_PROB_IDX + 3;
constexpr int META_IDX = DESIRE_STATE_IDX + DESIRE_LEN;
constexpr int POSE_IDX = META_IDX + OTHER_META_SIZE + DESIRE_PRED_SIZE;
constexpr int OUTPUT_SIZE =  POSE_IDX + POSE_SIZE;
#ifdef TEMPORAL
  constexpr int TEMPORAL_SIZE = 512;
#else
  constexpr int TEMPORAL_SIZE = 0;
#endif

// #define DUMP_YUV

Eigen::Matrix<float, MODEL_PATH_DISTANCE, POLYFIT_DEGREE - 1> vander;
float X_IDXS[TRAJECTORY_SIZE];
float T_IDXS[TRAJECTORY_SIZE];

void model_init(ModelState* s, cl_device_id device_id, cl_context context, int temporal) {
  frame_init(&s->frame, MODEL_WIDTH, MODEL_HEIGHT, device_id, context);
  s->input_frames = std::make_unique<float[]>(MODEL_FRAME_SIZE * 2);

  constexpr int output_size = OUTPUT_SIZE + TEMPORAL_SIZE;
  s->output = std::make_unique<float[]>(output_size);
  s->m = std::make_unique<DefaultRunModel>("../../models/supercombo.dlc", &s->output[0], output_size, USE_GPU_RUNTIME);

#ifdef TEMPORAL
  assert(temporal);
  s->m->addRecurrent(&s->output[OUTPUT_SIZE], TEMPORAL_SIZE);
#endif

#ifdef DESIRE
  s->prev_desire = std::make_unique<float[]>(DESIRE_LEN);
  s->pulse_desire = std::make_unique<float[]>(DESIRE_LEN);
  s->m->addDesire(s->pulse_desire.get(), DESIRE_LEN);
#endif

#ifdef TRAFFIC_CONVENTION
  const int idx = Params().read_db_bool("IsRHD") ? 1 : 0;
  s->traffic_convention[idx] = 1.0;
  s->m->addTrafficConvention(s->traffic_convention, TRAFFIC_CONVENTION_LEN);
#endif

  // Build Vandermonde matrix
  for(int i = 0; i < TRAJECTORY_SIZE; i++) {
    for(int j = 0; j < POLYFIT_DEGREE - 1; j++) {
      X_IDXS[i] = (TRAJECTORY_DISTANCE/1024.0) * (pow(i,2));
      T_IDXS[i] = (TRAJECTORY_TIME/1024.0) * (pow(i,2));
      vander(i, j) = pow(X_IDXS[i], POLYFIT_DEGREE-j-1);
    }
  }
}

ModelDataRaw model_eval_frame(ModelState* s, cl_command_queue q,
                           cl_mem yuv_cl, int width, int height,
                           mat3 transform, void* sock,
                           float *desire_in) {
#ifdef DESIRE
  if (desire_in != NULL) {
    for (int i = 1; i < DESIRE_LEN; i++) {
      // Model decides when action is completed
      // so desire input is just a pulse triggered on rising edge
      if (desire_in[i] - s->prev_desire[i] > .99) {
        s->pulse_desire[i] = desire_in[i];
      } else {
        s->pulse_desire[i] = 0.0;
      }
      s->prev_desire[i] = desire_in[i];
    }
  }
#endif

  //for (int i = 0; i < OUTPUT_SIZE + TEMPORAL_SIZE; i++) { printf("%f ", s->output[i]); } printf("\n");

  float *new_frame_buf = frame_prepare(&s->frame, q, yuv_cl, width, height, transform);
  memmove(&s->input_frames[0], &s->input_frames[MODEL_FRAME_SIZE], sizeof(float)*MODEL_FRAME_SIZE);
  memmove(&s->input_frames[MODEL_FRAME_SIZE], new_frame_buf, sizeof(float)*MODEL_FRAME_SIZE);
  s->m->execute(&s->input_frames[0], MODEL_FRAME_SIZE*2);

  #ifdef DUMP_YUV
    FILE *dump_yuv_file = fopen("/sdcard/dump.yuv", "wb");
    fwrite(new_frame_buf, MODEL_HEIGHT*MODEL_WIDTH*3/2, sizeof(float), dump_yuv_file);
    fclose(dump_yuv_file);
    assert(1==2);
  #endif

  clEnqueueUnmapMemObject(q, s->frame.net_input, (void*)new_frame_buf, 0, NULL, NULL);

  // net outputs
  ModelDataRaw net_outputs = {
      .plan = &s->output[PLAN_IDX],
      .lane_lines = &s->output[LL_IDX],
      .lane_lines_prob = &s->output[LL_PROB_IDX],
      .road_edges = &s->output[RE_IDX],
      .lead = &s->output[LEAD_IDX],
      .lead_prob = &s->output[LEAD_PROB_IDX],
      .meta = &s->output[DESIRE_STATE_IDX],
      .pose = &s->output[POSE_IDX]};
  return net_outputs;
}

void model_free(ModelState* s) {
  frame_free(&s->frame);
}

void poly_fit(float *in_pts, float *in_stds, float *out, int valid_len) {
  // References to inputs
  Eigen::Map<Eigen::Matrix<float, Eigen::Dynamic, 1> > pts(in_pts, valid_len);
  Eigen::Map<Eigen::Matrix<float, Eigen::Dynamic, 1> > std(in_stds, valid_len);
  Eigen::Map<Eigen::Matrix<float, POLYFIT_DEGREE - 1, 1> > p(out, POLYFIT_DEGREE - 1);

  float y0 = pts[0];
  pts = pts.array() - y0;

  // Build Least Squares equations
  Eigen::Matrix<float, Eigen::Dynamic, POLYFIT_DEGREE - 1> lhs = vander.topRows(valid_len).array().colwise() / std.array();
  Eigen::Matrix<float, Eigen::Dynamic, 1> rhs = pts.array() / std.array();

  // Improve numerical stability
  Eigen::Matrix<float, POLYFIT_DEGREE - 1, 1> scale = 1. / (lhs.array()*lhs.array()).sqrt().colwise().sum();
  lhs = lhs * scale.asDiagonal();

  // Solve inplace
  p = lhs.colPivHouseholderQr().solve(rhs);

  // Apply scale to output
  p = p.transpose() * scale.asDiagonal();
  out[3] = y0;
}

static const float *get_plan_data(float *plan) {
  const float *data = &plan[0];
  for (int i = 1; i < PLAN_MHP_N; ++i) {
    if (const float *cur = &plan[i * PLAN_MHP_GROUP_SIZE-1]; *cur > *data) {
      data = cur;
    }
  }
  return data;
}

static const float *get_lead_data(const float *lead, int t_offset) {
  const float *data = &lead[0];
  for (int i = 1; i < LEAD_MHP_N; ++i) {
    const float *cur = &lead[i * LEAD_MHP_GROUP_SIZE + t_offset - LEAD_MHP_SELECTION];
    if (*cur > *data) {
      data = cur;
    }
  }
  return data;
}

void fill_path(cereal::ModelData::PathData::Builder path, const float *data, const float prob, float valid_len, int valid_len_idx, int ll_idx) {
  float points[TRAJECTORY_SIZE] = {};
  float stds[TRAJECTORY_SIZE] = {};
  float poly[POLYFIT_DEGREE] = {};

  for (int i=0; i<TRAJECTORY_SIZE; i++) {
    // negative sign because mpc has left positive
    if (ll_idx == 0) {
      points[i] = -data[30 * i + 16];
      stds[i] = exp(data[30 * (33 + i) + 16]);
    } else {
      points[i] = -data[2 * 33 * ll_idx + 2 * i];
      stds[i] = exp(data[2 * 33 * (4 + ll_idx) + 2 * i]);
    }
  }
  poly_fit(points, stds, poly, valid_len_idx);

  path.setPoly(poly);
  path.setProb(prob);
  path.setStd(stds[0]);
  path.setValidLen(valid_len);
}

void fill_lead_v2(cereal::ModelDataV2::LeadDataV2::Builder lead, const float *lead_data, float prob, float t) {
  const float *data = get_lead_data(lead_data, t);
  float xyva[LEAD_MHP_VALS], xyva_stds[LEAD_MHP_VALS];
  for (int i = 0; i < LEAD_MHP_VALS; i++) {
    xyva[i] = data[LEAD_MHP_VALS + i];
    xyva_stds[i] = exp(data[LEAD_MHP_VALS + i]);
  }
  lead.setT(t);
  lead.setProb(sigmoid(prob));
  lead.setXyva(xyva);
  lead.setXyvaStd(xyva_stds);
}

void fill_lead(cereal::ModelData::LeadData::Builder lead, const float *lead_data, const float *prob, int t_offset) {
  const float *data = get_lead_data(lead_data, t_offset);
  lead.setProb(sigmoid(prob[t_offset]));
  lead.setDist(data[0]);
  lead.setStd(exp(data[LEAD_MHP_VALS]));
  // TODO make all msgs same format
  lead.setRelY(-data[1]);
  lead.setRelYStd(exp(data[LEAD_MHP_VALS + 1]));
  lead.setRelVel(data[2]);
  lead.setRelVelStd(exp(data[LEAD_MHP_VALS + 2]));
  lead.setRelA(data[3]);
  lead.setRelAStd(exp(data[LEAD_MHP_VALS + 3]));
}

template <class MetaBuilder>
void fill_meta(MetaBuilder meta, const float *meta_data) {
  float desire_state_softmax[DESIRE_LEN];
  float desire_pred_softmax[4*DESIRE_LEN];
  softmax(&meta_data[0], desire_state_softmax, DESIRE_LEN);
  for (int i=0; i<4; i++) {
    softmax(&meta_data[DESIRE_LEN + OTHER_META_SIZE + i*DESIRE_LEN],
            &desire_pred_softmax[i*DESIRE_LEN], DESIRE_LEN);
  }
  meta.setDesireState(desire_state_softmax);
  meta.setEngagedProb(sigmoid(meta_data[DESIRE_LEN]));
  meta.setGasDisengageProb(sigmoid(meta_data[DESIRE_LEN + 1]));
  meta.setBrakeDisengageProb(sigmoid(meta_data[DESIRE_LEN + 2]));
  meta.setSteerOverrideProb(sigmoid(meta_data[DESIRE_LEN + 3]));
  meta.setDesirePrediction(desire_pred_softmax);
}

void fill_xyzt(cereal::ModelDataV2::XYZTData::Builder xyzt, const float * data,
               int columns, int column_offset, float * plan_t) {
  float x[TRAJECTORY_SIZE] = {}, y[TRAJECTORY_SIZE] = {}, z[TRAJECTORY_SIZE] = {};
  //float x_std_arr[TRAJECTORY_SIZE];
  //float y_std_arr[TRAJECTORY_SIZE];
  //float z_std_arr[TRAJECTORY_SIZE];
  float t[TRAJECTORY_SIZE] = {};
  for (int i=0; i<TRAJECTORY_SIZE; ++i) {
    // column_offset == -1 means this data is X indexed not T indexed
    if (column_offset >= 0) {
      t[i] = T_IDXS[i];
      x[i] = data[i*columns + 0 + column_offset];
      //x_std_arr[i] = data[columns*(TRAJECTORY_SIZE + i) + 0 + column_offset];
    } else {
      t[i] = plan_t[i];
      x[i] = X_IDXS[i];
      //x_std_arr[i] = NAN;
    }
    y[i] = data[i*columns + 1 + column_offset];
    //y_std[i] = data[columns*(TRAJECTORY_SIZE + i) + 1 + column_offset];
    z[i] = data[i*columns + 2 + column_offset];
    //z_std_arr[i] = data[columns*(TRAJECTORY_SIZE + i) + 2 + column_offset];
  }
  xyzt.setX(x);
  xyzt.setY(y);
  xyzt.setZ(z);
  //xyzt.setXStd(x_std);
  //xyzt.setYStd(y_std);
  //xyzt.setZStd(z_std);
  xyzt.setT(t);
}

void fill_model(cereal::ModelDataV2::Builder &framed, const ModelDataRaw &net_outputs) {
  // plan
  const float *best_plan = get_plan_data(net_outputs.plan);
  float plan_t[TRAJECTORY_SIZE];
  for (int i=0; i<TRAJECTORY_SIZE; i++) {
    plan_t[i] = best_plan[i*PLAN_MHP_COLUMNS + 15];
  }

  fill_xyzt(framed.initPosition(), best_plan, PLAN_MHP_COLUMNS, 0, plan_t);
  fill_xyzt(framed.initVelocity(), best_plan, PLAN_MHP_COLUMNS, 3, plan_t);
  fill_xyzt(framed.initOrientation(), best_plan, PLAN_MHP_COLUMNS, 9, plan_t);
  fill_xyzt(framed.initOrientationRate(), best_plan, PLAN_MHP_COLUMNS, 12, plan_t);

  // lane lines
  auto lane_lines = framed.initLaneLines(4);
  float lane_line_probs[4], lane_line_stds[4];
  for (int i = 0; i < 4; i++) {
    fill_xyzt(lane_lines[i], &net_outputs.lane_lines[i*TRAJECTORY_SIZE*2], 2, -1, plan_t);
    lane_line_probs[i] = sigmoid(net_outputs.lane_lines_prob[i]);
    lane_line_stds[i] = exp(net_outputs.lane_lines[2*TRAJECTORY_SIZE*(4 + i)]);
  }
  framed.setLaneLineProbs(lane_line_probs);
  framed.setLaneLineStds(lane_line_stds);

  // road edges
  auto road_edges = framed.initRoadEdges(2);
  float road_edge_stds[2];
  for (int i = 0; i < 2; i++) {
    fill_xyzt(road_edges[i], &net_outputs.road_edges[i*TRAJECTORY_SIZE*2], 2, -1, plan_t);
    road_edge_stds[i] = exp(net_outputs.road_edges[2*TRAJECTORY_SIZE*(2 + i)]);
  }
  framed.setRoadEdgeStds(road_edge_stds);

  // meta
  fill_meta(framed.initMeta(), net_outputs.meta);

  // leads
  auto leads = framed.initLeads(LEAD_MHP_SELECTION);
  const float t_offsets[LEAD_MHP_SELECTION] = {0.0, 2.0, 4.0};
  for (int t_offset = 0; t_offset < LEAD_MHP_SELECTION; t_offset++) {
    fill_lead_v2(leads[t_offset], net_outputs.lead, net_outputs.lead_prob[t_offset], t_offsets[t_offset]);
  }
}

void fill_model(cereal::ModelData::Builder &framed, const ModelDataRaw &net_outputs) {
  // Find the distribution that corresponds to the most probable plan
  const float *best_plan = get_plan_data(net_outputs.plan);
  // x pos at 10s is a good valid_len
  float valid_len = 0;
  for (int i=1; i<TRAJECTORY_SIZE; i++) {
    if (const float len = best_plan[30*i]; len >= valid_len){
      valid_len = len;
    }
  }
  // clamp to 10 and MODEL_PATH_DISTANCE
  valid_len = fmin(MODEL_PATH_DISTANCE, fmax(MIN_VALID_LEN, valid_len));
  int valid_len_idx = 0;
  for (int i=1; i<TRAJECTORY_SIZE; i++) {
    if (valid_len >= X_IDXS[valid_len_idx]){
      valid_len_idx = i;
    }
  }
  fill_path(framed.initPath(), best_plan, 1.0, valid_len, valid_len_idx, 0);
  fill_path(framed.initLeftLane(), net_outputs.lane_lines, net_outputs.lane_lines_prob[1], valid_len, valid_len_idx, 1);
  fill_path(framed.initRightLane(), net_outputs.lane_lines, net_outputs.lane_lines_prob[2], valid_len, valid_len_idx, 2);

  fill_lead(framed.initLead(), net_outputs.lead, net_outputs.lead_prob, 0);
  fill_lead(framed.initLeadFuture(), net_outputs.lead, net_outputs.lead_prob, 1);

  fill_meta(framed.initMeta(), net_outputs.meta);
}

void model_publish(PubMaster &pm, uint32_t vipc_frame_id, uint32_t frame_id, float frame_drop,
                   const ModelDataRaw &net_outputs, const float *raw_pred, uint64_t timestamp_eof,
                   float model_execution_time) {
  const uint32_t frame_age = (frame_id > vipc_frame_id) ? (frame_id - vipc_frame_id) : 0;
  auto do_publish = [&](auto init_model_func, const char *pub_name) {
    MessageBuilder msg;
    auto framed = (msg.initEvent().*(init_model_func))();
    framed.setFrameId(vipc_frame_id);
    framed.setFrameAge(frame_age);
    framed.setFrameDropPerc(frame_drop * 100);
    framed.setTimestampEof(timestamp_eof);
    framed.setModelExecutionTime(model_execution_time);
    if (send_raw_pred) {
      framed.setRawPred(kj::arrayPtr((const uint8_t *)raw_pred, (OUTPUT_SIZE + TEMPORAL_SIZE) * sizeof(float)));
    }
    fill_model(framed, net_outputs);
    pm.send(pub_name, msg);
  };
  do_publish(&cereal::Event::Builder::initModel, "model");
  do_publish(&cereal::Event::Builder::initModelV2, "modelV2");
}

void posenet_publish(PubMaster &pm, uint32_t vipc_frame_id, uint32_t vipc_dropped_frames,
                     const ModelDataRaw &net_outputs, uint64_t timestamp_eof) {
  float trans[3], trans_std[3];
  float rot[3], rot_std[3];

  for (int i =0; i < 3; i++) {
    trans[i] = net_outputs.pose[i];
    trans_std[i] = exp(net_outputs.pose[6 + i]);

    rot[i] = net_outputs.pose[3 + i];
    rot_std[i] = exp(net_outputs.pose[9 + i]);
  }

  MessageBuilder msg;
  auto posenetd = msg.initEvent(vipc_dropped_frames < 1).initCameraOdometry();
  posenetd.setTrans(trans);
  posenetd.setRot(rot);
  posenetd.setTransStd(trans_std);
  posenetd.setRotStd(rot_std);

  posenetd.setTimestampEof(timestamp_eof);
  posenetd.setFrameId(vipc_frame_id);

  pm.send("cameraOdometry", msg);
}
