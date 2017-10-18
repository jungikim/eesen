// net/affine-trans-layer.h

// Copyright 2011-2014  Brno University of Technology (author: Karel Vesely)
//                2015  Yajie Miao, Hang Su
//                

// See ../../COPYING for clarification regarding multiple authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// THIS CODE IS PROVIDED *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED
// WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE,
// MERCHANTABLITY OR NON-INFRINGEMENT.
// See the Apache 2 License for the specific language governing permissions and
// limitations under the License.


#ifndef EESEN_AFFINE_TRANS_LAYER_H_
#define EESEN_AFFINE_TRANS_LAYER_H_

#include "net/layer.h"
#include "net/trainable-layer.h"
#include "net/utils-functions.h"
#include "gpucompute/cuda-math.h"

namespace eesen {

class AffineTransform : public TrainableLayer {
 public:
  AffineTransform(int32 dim_in, int32 dim_out) 
    : TrainableLayer(dim_in, dim_out), 
      linearity_(dim_out, dim_in), bias_(dim_out),
      linearity_corr_(dim_out, dim_in), bias_corr_(dim_out),
      learn_rate_coef_(1.0), max_grad_(0.0),
      adaBuffersInitialized(false)
  { }
  ~AffineTransform()
  { }

  Layer* Copy() const { return new AffineTransform(*this); }
  LayerType GetType() const { return l_Affine_Transform; }
  LayerType GetTypeNonParal() const { return l_Affine_Transform; }
 
  void InitData(std::istream &is) {
    // define options
    float param_range = 0.02, max_grad = 0.0;
    float learn_rate_coef = 1.0;
    // parse config
    std::string token; 
    while (!is.eof()) {
      ReadToken(is, false, &token); 
      /**/ if (token == "<ParamRange>") ReadBasicType(is, false, &param_range);
      else if (token == "<LearnRateCoef>") ReadBasicType(is, false, &learn_rate_coef);
      else if (token == "<MaxGrad>") ReadBasicType(is, false, &max_grad);
      else KALDI_ERR << "Unknown token " << token << ", a typo in config?"
                     << " (ParamStddev|BiasMean|BiasRange|LearnRateCoef|BiasLearnRateCoef)";
      is >> std::ws; // eat-up whitespace
    }

    // initialize
    linearity_.Resize(output_dim_, input_dim_, kUndefined); linearity_.InitRandUniform(param_range);
    bias_.Resize(output_dim_, kUndefined); bias_.InitRandUniform(param_range);
    
    //
    learn_rate_coef_ = learn_rate_coef;
    max_grad_ = max_grad;
  }

  void InitAdaBuffers() {
    // initialize Ada vars
    linearity_corr_accu.Resize(output_dim_, input_dim_, kUndefined); linearity_corr_accu.Set(0.0);
    bias_corr_accu.Resize(output_dim_, kUndefined); bias_corr_accu.Set(0.0);
    linearity_corr_accu_scale.Resize(output_dim_, input_dim_, kUndefined); linearity_corr_accu_scale.Set(0.0);
    bias_corr_accu_scale.Resize(output_dim_, kUndefined); bias_corr_accu_scale.Set(0.0);
    adaBuffersInitialized = true;
  }

  void ReadData(std::istream &is, bool binary) {
    
    adaBuffersInitialized = false;
    
    // optional learning-rate coefs
    if ('<' == Peek(is, binary)) {
      ExpectToken(is, binary, "<LearnRateCoef>");
      ReadBasicType(is, binary, &learn_rate_coef_);
    }

    if ('<' == Peek(is, binary)) {
      ExpectToken(is, binary, "<MaxGrad>");
      ReadBasicType(is, binary, &max_grad_);
    }

    // optionally read in accumolators for AdaGrad and RMSProp
    if ('<' == Peek(is, binary)) {
      ExpectToken(is, binary, "<AffineAccus>");
   
      InitAdaBuffers();

      linearity_corr_accu.Read(is, binary);
      bias_corr_accu.Read(is, binary);
    }

    // weights
    linearity_.Read(is, binary);
    bias_.Read(is, binary);

    KALDI_ASSERT(linearity_.NumRows() == output_dim_);
    KALDI_ASSERT(linearity_.NumCols() == input_dim_);
    KALDI_ASSERT(bias_.Dim() == output_dim_);
  }

  void WriteData(std::ostream &os, bool binary) const {
    WriteToken(os, binary, "<LearnRateCoef>");
    WriteBasicType(os, binary, learn_rate_coef_);
    WriteToken(os, binary, "<MaxGrad>");
    WriteBasicType(os, binary, max_grad_);

    // write out optional accumolators
    if(adaBuffersInitialized)
    {
        WriteToken(os, binary, "<AffineAccus>");
        linearity_corr_accu.Write(os, binary);
        bias_corr_accu.Write(os, binary);
    }

    // weights
    linearity_.Write(os, binary);
    bias_.Write(os, binary);
  }

  int32 NumParams() const { return linearity_.NumRows()*linearity_.NumCols() + bias_.Dim(); }
  
  void GetParams(Vector<BaseFloat>* wei_copy) const {
    wei_copy->Resize(NumParams());
    int32 linearity_num_elem = linearity_.NumRows() * linearity_.NumCols(); 
    wei_copy->Range(0,linearity_num_elem).CopyRowsFromMat(Matrix<BaseFloat>(linearity_));
    wei_copy->Range(linearity_num_elem, bias_.Dim()).CopyFromVec(Vector<BaseFloat>(bias_));
  }
  
  std::string Info() const {
    return std::string("\n  linearity") + MomentStatistics(linearity_) +
           "\n  bias" + MomentStatistics(bias_);
  }
  std::string InfoGradient() const {
    std::string extra = std::string("");
    if (adaBuffersInitialized)
    {
      extra += "\n  linearity_grad_accu" + MomentStatistics(linearity_corr_accu) +
               "\n  bias_grad_accu" + MomentStatistics(bias_corr_accu);
    }
    return std::string("\n  linearity_corr_") + MomentStatistics(linearity_corr_) + 
                       "\n  bias_corr_" + MomentStatistics(bias_corr_) + extra;
  }


  void PropagateFnc(const CuMatrixBase<BaseFloat> &in, CuMatrixBase<BaseFloat> *out) {
    // precopy bias
    out->AddVecToRows(1.0, bias_, 0.0);
    // multiply by weights^t
    out->AddMatMat(1.0, in, kNoTrans, linearity_, kTrans, 1.0);
  }

  void BackpropagateFnc(const CuMatrixBase<BaseFloat> &in, const CuMatrixBase<BaseFloat> &out,
                        const CuMatrixBase<BaseFloat> &out_diff, CuMatrixBase<BaseFloat> *in_diff) {
    // multiply error derivative by weights
    in_diff->AddMatMat(1.0, out_diff, kNoTrans, linearity_, kNoTrans, 0.0);
  }

  void Update(const CuMatrixBase<BaseFloat> &input, const CuMatrixBase<BaseFloat> &diff, const UpdateRule
  rule=sgd_update) {

    // we use following hyperparameters from the option class
    BaseFloat lr = opts_.learn_rate;
    const BaseFloat mmt = opts_.momentum;
    
    // compute gradient (incl. momentum)
    linearity_corr_.AddMatMat(1.0, diff, kTrans, input, kNoTrans, mmt);
    bias_corr_.AddRowSumMat(1.0, diff, mmt);
    
    // clip gradients
    if (max_grad_ > 0) {
      linearity_corr_.ApplyFloor(-max_grad_); linearity_corr_.ApplyCeiling(max_grad_);
      bias_corr_.ApplyFloor(-max_grad_); bias_corr_.ApplyCeiling(max_grad_);
    }
    
    if (rule==sgd_update) {
      // update
      lr *= learn_rate_coef_;
      linearity_.AddMat(-lr, linearity_corr_);
      bias_.AddVec(-lr, bias_corr_);
    } else if (rule==adagrad_update || rule==rmsprop_update) {

      if (!adaBuffersInitialized) {
        InitAdaBuffers();
        adaBuffersInitialized=true;
      }

      // update the accumolatorsi
      if (rule==adagrad_update)
      {
        AdagradAccuUpdate(linearity_corr_accu, linearity_corr_, linearity_corr_accu_scale);
        AdagradAccuUpdate(bias_corr_accu, bias_corr_, bias_corr_accu_scale);
      }else {
        RMSPropAccuUpdate(linearity_corr_accu, linearity_corr_, linearity_corr_accu_scale);
        RMSPropAccuUpdate(bias_corr_accu, bias_corr_, bias_corr_accu_scale);
      }
      // calculate 1.0 / sqrt(accu + epsilon)
      AdagradScaleCompute(linearity_corr_accu_scale, linearity_corr_accu);
      AdagradScaleCompute(bias_corr_accu_scale, bias_corr_accu);
      // update the parameters
      linearity_.AddMatMatElements(-lr, linearity_corr_accu_scale, linearity_corr_, 1.0);
      bias_.AddVecVec(-lr, bias_corr_accu_scale, bias_corr_, 1.0);
    }
  }
  
  void Scale(BaseFloat scale) {
    linearity_.Scale(scale);
    bias_.Scale(scale);
  }

  void Add(BaseFloat scale, const TrainableLayer & layer_other) {
    const AffineTransform *other = dynamic_cast<const AffineTransform*>(&layer_other);
    linearity_.AddMat(scale, other->linearity_);
    bias_.AddVec(scale, other->bias_);
  }

  void SetBias(const CuVectorBase<BaseFloat>& bias) {
    KALDI_ASSERT(bias.Dim() == bias_.Dim());
    bias_.CopyFromVec(bias);
  }

  void SetDropFactor(BaseFloat dropfactor) {
      //TODO
  }

  const CuMatrixBase<BaseFloat>& GetLinearity() const {
    return linearity_;
  }

  void SetLinearity(const CuMatrixBase<BaseFloat>& linearity) {
    KALDI_ASSERT(linearity.NumRows() == linearity_.NumRows());
    KALDI_ASSERT(linearity.NumCols() == linearity_.NumCols());
    linearity_.CopyFromMat(linearity);
  }

  const CuVectorBase<BaseFloat>& GetBiasCorr() const {
    return bias_corr_;
  }

  const CuMatrixBase<BaseFloat>& GetLinearityCorr() const {
    return linearity_corr_;
  }


 private:
  CuMatrix<BaseFloat> linearity_;
  CuVector<BaseFloat> bias_;

  CuMatrix<BaseFloat> linearity_corr_;
  CuVector<BaseFloat> bias_corr_;

  CuMatrix<BaseFloat> linearity_corr_accu;
  CuVector<BaseFloat> bias_corr_accu;

  CuMatrix<BaseFloat> linearity_corr_accu_scale;
  CuVector<BaseFloat> bias_corr_accu_scale;

  BaseFloat learn_rate_coef_;
  BaseFloat max_grad_;

  bool adaBuffersInitialized;
};

} // namespace eesen

#endif
