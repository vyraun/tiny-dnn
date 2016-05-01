/*
    Copyright (c) 2013, Taiga Nomi
    All rights reserved.
    
    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
    names of its contributors may be used to endorse or promote products
    derived from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY 
    EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED 
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY 
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES 
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; 
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND 
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS 
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#pragma once
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <iterator>
#include <iomanip>
#include <map>
#include <set>

#include "tiny_cnn/nodes.h"
#include "tiny_cnn/util/util.h"
#include "tiny_cnn/lossfunctions/loss_function.h"
#include "tiny_cnn/activations/activation_function.h"

namespace tiny_cnn {

struct result {
    result() : num_success(0), num_total(0) {}

    double accuracy() const {
        return num_success * 100.0 / num_total;
    }

    template <typename Char, typename CharTraits>
    void print_summary(std::basic_ostream<Char, CharTraits>& os) const {
        os << "accuracy:" << accuracy() << "% (" << num_success << "/" << num_total << ")" << std::endl;
    }

    template <typename Char, typename CharTraits>
    void print_detail(std::basic_ostream<Char, CharTraits>& os) {
        print_summary(os);
        auto all_labels = labels();

        os << std::setw(5) << "*" << " ";
        for (auto c : all_labels) 
            os << std::setw(5) << c << " ";
        os << std::endl;

        for (auto r : all_labels) {
            os << std::setw(5) << r << " ";           
            for (auto c : all_labels) 
                os << std::setw(5) << confusion_matrix[r][c] << " ";
            os << std::endl;
        }
    }

    std::set<label_t> labels() const {
        std::set<label_t> all_labels;
        for (auto r : confusion_matrix) {
            all_labels.insert(r.first);
            for (auto c : r.second)
                all_labels.insert(c.first);
        }
        return all_labels;
    }

    int num_success;
    int num_total;
    std::map<label_t, std::map<label_t, int> > confusion_matrix;
};

enum grad_check_mode {
    GRAD_CHECK_ALL, ///< check all elements of weights
    GRAD_CHECK_RANDOM ///< check 10 randomly selected weights
};

template <typename NetType>
class network;

template <typename Layer>
network<sequential>& operator << (network<sequential>& n, Layer&& l);

template<typename NetType>
class network
{
public:
    explicit network(const std::string& name = "") : name_(name) {}

    std::string  name() const           { return name_; }

    /**
     * explicitly initialize weights of all layers
     **/
    void         init_weight()          { layers_.init_weight(); }

    /**
     * executes forward-propagation and returns output
     **/
    vec_t        predict(const vec_t& in) { return fprop(in); }

    /**
     * executes forward-propagation and returns maximum output
     **/
    float_t      predict_max_value(const vec_t& in) {
        return fprop_max(in);
    }

    /**
     * executes forward-propagation and returns maximum output index
     **/
    label_t      predict_label(const vec_t& in) {
        return fprop_max_index(in);
    }

    /**
     * executes forward-propagation and returns output
     *
     * @param in input value range(double[], std::vector<double>, std::list<double> etc)
     **/
    template <typename Range>
    vec_t        predict(const Range& in) {
        using std::begin; // for ADL
        using std::end;
        return predict(vec_t(begin(in), end(in)));
    }

    /**
     * training conv-net
     *
     * @param in                 array of input data
     * @param t                  array of training signals(label or vector)
     * @param epoch              number of training epochs
     * @param on_batch_enumerate callback for each mini-batch enumerate
     * @param on_epoch_enumerate callback for each epoch 
     * @param reset_weights      reset all weights or keep current
     * @param n_threads          number of tasks
     * @param t_cost             target costs (leave to nullptr in order to assume equal cost for every target)
     */
    template <typename Error, typename Optimizer, typename OnBatchEnumerate, typename OnEpochEnumerate, typename T>
    bool train(Optimizer&                optimizer,
               const std::vector<vec_t>& in,
               const std::vector<T>&     t,
               size_t                    batch_size,
               int                       epoch,
               OnBatchEnumerate          on_batch_enumerate,
               OnEpochEnumerate          on_epoch_enumerate,

               const bool                reset_weights = true,
               const int                 n_threads = CNN_TASK_SIZE,
               const std::vector<vec_t>* t_cost = nullptr
               )
    {
        //check_training_data(in, t);
        check_target_cost_matrix(t, t_cost);
        set_netphase(net_phase::train);
        net_.setup(reset_weights, std::min(n_threads, (int)batch_size));

        for (auto n : net_)
            n->set_parallelize(batch_size < CNN_TASK_SIZE);
        optimizer.reset();

        for (int iter = 0; iter < epoch; iter++) {
            for (size_t i = 0; i < in.size(); i+=batch_size) {
                train_once<Error>(optimizer, &in[i], &t[i],
                           static_cast<int>(std::min(batch_size, in.size() - i)),
                           n_threads,
                           get_target_cost_sample_pointer(t_cost, i));
                on_batch_enumerate();

                //if (i % 100 == 0 && layers_.is_exploded()) {
                //    std::cout << "[Warning]Detected infinite value in weight. stop learning." << std::endl;
                //    return false;
                //}
            }
            on_epoch_enumerate();
        }
        return true;
    }

    /**
     * training conv-net without callback
     **/
    template<typename Error, typename Optimizer, typename T>
    bool train(Optimizer& optimizer, const std::vector<vec_t>& in, const std::vector<T>& t, size_t batch_size = 1, int epoch = 1) {
        set_netphase(net_phase::train);
        return train<Error>(optimizer, in, t, batch_size, epoch, nop, nop);
    }

    /**
     * set the netphase to train or test
     * @param phase phase of network, could be train or test
     */
    void set_netphase(net_phase phase)
    {
        for (auto n : net_) {
            n->set_context(phase);
        }
    }

    /**
     * test and generate confusion-matrix for classification task
     **/
    result test(const std::vector<vec_t>& in, const std::vector<label_t>& t) {
        result test_result;
        set_netphase(net_phase::test);
        for (size_t i = 0; i < in.size(); i++) {
            const label_t predicted = fprop_max_index(in[i]);
            const label_t actual = t[i];

            if (predicted == actual) test_result.num_success++;
            test_result.num_total++;
            test_result.confusion_matrix[predicted][actual]++;
        }
        return test_result;
    }

    std::vector<vec_t> test(const std::vector<vec_t>& in)
     {
            std::vector<vec_t> test_result(in.size());
            set_netphase(net_phase::test);
            for_i(in.size(), [&](int i)
            {
                test_result[i] = predict(in[i]);
            });
            return test_result;
    }

    /**
     * calculate loss value (the smaller, the better) for regression task
     **/
    float_t get_loss(const std::vector<vec_t>& in, const std::vector<vec_t>& t) {
        float_t sum_loss = float_t(0);

        for (size_t i = 0; i < in.size(); i++) {
            const vec_t predicted = predict(in[i]);
            sum_loss += get_loss(predict(in[i]), t[i]);
        }
        return sum_loss;
    }

    /**
     * save network weights into stream
     * @attention this saves only network *weights*, not network configuration
     **/
    void save(std::ostream& os) const {
        os.precision(std::numeric_limits<tiny_cnn::float_t>::digits10);
        net_.save(os);
    }

    /**
     * load network weights from stream
     * @attention this loads only network *weights*, not network configuration
     **/
    void load(std::istream& is) {
        is.precision(std::numeric_limits<tiny_cnn::float_t>::digits10);
        net_.load(is);
    }
    
    /**
     * load network weights from filepath, 30 times faster than stream reading
     * @attention this loads only network *weights*, not network configuration
     **/
    void fast_load(const char* filepath) {
		FILE* stream = fopen(filepath, "r");
		//double* temp = new double[param_num];
		//const double* data = temp;
		std::vector<double> data;
		double temp;
		while (fscanf(stream, "%lf", &temp) > 0)
			data.push_back(temp);
		fclose(stream);

		net_.load(data);
	}

protected:
    float_t fprop_max(const vec_t& in, int idx = 0) {
        const vec_t& prediction = fprop(in, idx);
        return *std::max_element(std::begin(prediction), std::end(prediction));
    }

    label_t fprop_max_index(const vec_t& in, int idx = 0) {
        return label_t(max_index(fprop(in, idx)));
    }
private:

    template <typename Layer>
    friend network<sequential>& operator << (network<sequential>& n, Layer&& l);

    void label2vector(const label_t* t, int num, std::vector<vec_t> *vec) const {
        cnn_size_t outdim = out_dim();

        assert(num > 0);
        assert(outdim > 0);

        vec->reserve(num);
        for (int i = 0; i < num; i++) {
            assert(t[i] < outdim);
            vec->emplace_back(outdim, net_.target_value_min());
            vec->back()[t[i]] = net_.target_value_max();
        }
    }

    /**
     * train on one minibatch
     *
     * @param size is the number of data points to use in this batch
     */
    template <typename E, typename Optimizer>
    void train_once(Optimizer& optimizer, const vec_t* in, const label_t* t, int size, const int nbThreads, const vec_t* t_cost) {
        std::vector<vec_t> v;
        label2vector(t, size, &v);
        train_once<E>(optimizer, in, &v[0], size, nbThreads, t_cost);
    }

    /**
     * train on one minibatch
     *
     * @param size is the number of data points to use in this batch
     */
    template <typename E, typename Optimizer>
    void train_once(Optimizer& optimizer, const vec_t* in, const vec_t* t, int size, const int nbThreads, const vec_t* t_cost) {
        if (size == 1) {
            bprop<E>(fprop(in[0]), t[0], 0, t_cost);
            net_.update_weights(&optimizer, 1, 1);
        } else {
            train_onebatch<E>(optimizer, in, t, size, nbThreads, t_cost);
        }
    }   

    /** 
     * trains on one minibatch, i.e. runs forward and backward propagation to calculate
     * the gradient of the loss function with respect to the network parameters (weights),
     * then calls the optimizer algorithm to update the weights
     *
     * @param batch_size the number of data points to use in this batch 
     */
    template <typename E, typename Optimizer>
    void train_onebatch(Optimizer& optimizer, const vec_t* in, const vec_t* t, int batch_size, const int num_tasks, const vec_t* t_cost) {
        int num_threads = std::min(batch_size, num_tasks);

        net_.set_worker_count(num_threads);

        // number of data points to use in each thread
        int data_per_thread = (batch_size + num_threads - 1) / num_threads;

        // i is the thread / worker index
        for_i(num_threads, [&](int i) {
            int start_index = i * data_per_thread;
            int end_index = std::min(batch_size, start_index + data_per_thread);

            // loop over data points in this batch assigned to thread i
            for (int j = start_index; j < end_index; ++j)
                bprop<E>(fprop(in[j], i), t[j], i, t_cost ? &(t_cost[j]) : nullptr);
        }, 1);
        
        // merge all dW and update W by optimizer
        net_.update_weights(&optimizer, num_threads, batch_size);
    }

    void calc_hessian(const std::vector<vec_t>& in, const std::vector<vec_t>* t_cost, int size_initialize_hessian = 500) {
        int size = std::min((int)in.size(), size_initialize_hessian);

        for (int i = 0; i < size; i++)
            bprop_2nd(fprop(in[i]), get_target_cost_sample_pointer(t_cost, i));

        layers_.divide_hessian(size);
    }

    /**
     * @param  h the activation function at the output of the last layer
     * @return true if the combination of the loss function E and the last layer output activation
     *         function h is such that dE / da = (dE/dY) * (dy/da) = y - target
     */
    template<typename Activation>
    bool is_canonical_link(const Activation& h) {
        if (typeid(h) == typeid(activation::sigmoid) && typeid(E) == typeid(cross_entropy)) return true;
        if (typeid(h) == typeid(activation::tan_h) && typeid(E) == typeid(cross_entropy)) return true;
        if (typeid(h) == typeid(activation::identity) && typeid(E) == typeid(mse)) return true;
        if (typeid(h) == typeid(activation::softmax) && typeid(E) == typeid(cross_entropy_multiclass)) return true;
        return false;
    }

    vec_t fprop(const vec_t& in, int idx = 0) {
        if (in.size() != (size_t)in_dim())
            data_mismatch(**net_.begin(), in);

        return net_.forward({in}, idx)[0];
    }

    float_t get_loss(const vec_t& out, const vec_t& t) {
        float_t e = float_t(0);
        assert(out.size() == t.size());
        for(size_t i = 0; i < out.size(); i++){ e += E::f(out[i], t[i]); }
        return e;
    }

    cnn_size_t out_dim() const {
        return net_.out_data_size();
    }

    cnn_size_t in_dim() const {
        return net_.in_data_size();
    }

    void bprop_2nd(const vec_t& out, const vec_t* t_cost) {
        vec_t delta(out_dim());
        const activation::function& h = layers_.tail()->activation_function();

        if (is_canonical_link(h)) {
            for_i(out_dim(), [&](int i){ delta[i] = target_value_max() * h.df(out[i]);});
        } else {
            for_i(out_dim(), [&](int i){ delta[i] = target_value_max() * h.df(out[i]) * h.df(out[i]);}); // FIXME
        }

        if (t_cost) {
            // CHECKME - is this correct?
            for_i(out_dim(), [&](int i) { delta[i] *= (*t_cost)[i]; });
        }

        layers_.tail()->back_propagation_2nd(delta);
    }

    template <typename E>
    void bprop(const vec_t& out, const vec_t& t, int idx, const vec_t* t_cost) {
        vec_t delta = gradient<E>(out, t);

        if (t_cost) {
            for_i(out_dim(), [&](int i) { delta[i] *= (*t_cost)[i]; });
        }

        net_.backward({delta}, idx);
    }

    bool calc_delta(const vec_t* in, const vec_t* v, int data_size, vec_t& w, vec_t& dw, int check_index, double eps) {
        static const float_t delta = 1e-10;

        std::fill(dw.begin(), dw.end(), float_t(0));

        // calculate dw/dE by numeric
        float_t prev_w = w[check_index];

        w[check_index] = prev_w + delta;
        float_t f_p = float_t(0);
        for(int i = 0; i < data_size; i++) { f_p += get_loss(fprop(in[i]), v[i]); }

        float_t f_m = float_t(0);
        w[check_index] = prev_w - delta;
        for(int i = 0; i < data_size; i++) { f_m += get_loss(fprop(in[i]), v[i]); }

        float_t delta_by_numerical = (f_p - f_m) / (float_t(2) * delta);
        w[check_index] = prev_w;

        // calculate dw/dE by bprop
        for(int i = 0; i < data_size; i++){ bprop(fprop(in[i]), v[i], 0, nullptr); }

        float_t delta_by_bprop = dw[check_index];

        return std::abs(delta_by_bprop - delta_by_numerical) <= eps;
    }

    void check_t(size_t i, label_t t, cnn_size_t dim_out) {
        if (t >= dim_out) {
            std::ostringstream os;
            os << format_str("t[%u]=%u, dim(network output)=%u", i, t, dim_out) << std::endl;
            os << "in classification task, dim(network output) must be greater than max class id." << std::endl;
            if (dim_out == 1)
                os << std::endl << "(for regression, use vector<vec_t> instead of vector<label_t> for training signal)" << std::endl;

            throw nn_error("output dimension mismatch!\n " + os.str());
        }
    }

    void check_t(size_t i, const vec_t& t, cnn_size_t dim_out) {
        if (t.size() != dim_out)
            throw nn_error(format_str("output dimension mismatch!\n dim(target[%u])=%u, dim(network output size=%u", i, t.size(), dim_out));
    }

    template <typename T>
    void check_training_data(const std::vector<vec_t>& in, const std::vector<T>& t) {
        cnn_size_t dim_in = in_dim();
        cnn_size_t dim_out = out_dim();

        if (in.size() != t.size())
            throw nn_error("number of training data must be equal to label data");

        size_t num = in.size();

        for (size_t i = 0; i < num; i++) {
            if (in[i].size() != dim_in)
                throw nn_error(format_str("input dimension mismatch!\n dim(data[%u])=%d, dim(network input)=%u", i, in[i].size(), dim_in));

            check_t(i, t[i], dim_out);
        }
    }

    template <typename T>
    void check_target_cost_matrix(const std::vector<T>& t, const std::vector<vec_t>* t_cost) {
        if (t_cost != nullptr) {
            if (t.size() != t_cost->size()) {
                throw nn_error("if target cost is supplied, its length must equal that of target data");
            }

            for (size_t i = 0, end = t.size(); i < end; i++) {
                check_target_cost_element(t[i], t_cost->operator[](i));
            }
        }
    }

    // classification
    void check_target_cost_element(const label_t t, const vec_t& t_cost) {
        if (t >= t_cost.size()) {
            throw nn_error("if target cost is supplied for a classification task, some cost must be given for each distinct class label");
        }
    }

    // regression
    void check_target_cost_element(const vec_t& t, const vec_t& t_cost) {
        if (t.size() != t_cost.size()) {
            throw nn_error("if target cost is supplied for a regression task, its shape must be identical to the target data");
        }
    }

    inline const vec_t* get_target_cost_sample_pointer(const std::vector<vec_t>* t_cost, size_t i) {
        if (t_cost) {
            const std::vector<vec_t>& target_cost = *t_cost;
            assert(i < target_cost.size());
            return &(target_cost[i]);
        }
        else {
            return nullptr;
        }
    }

    std::string name_;
    NetType net_;
};

/**
 * @brief [cut an image in samples to be tested (slow)]
 * @details [long description]
 * 
 * @param data [pointer to the data]
 * @param rows [self explained]
 * @param cols [self explained]
 * @param sizepatch [size of the patch, such as the total number of pixel in the patch is sizepatch*sizepatch ]
 * @return [vector of vec_c (sample) to be passed to test function]
 */
inline std::vector<vec_t> image2vec(const float_t* data, const unsigned int  rows, const unsigned int cols, const unsigned int sizepatch, const unsigned int step=1)
{
    assert(step>0);
    std::vector<vec_t> res((cols-sizepatch)*(rows-sizepatch)/(step*step),vec_t(sizepatch*sizepatch));
        for_i((cols-sizepatch)*(rows-sizepatch)/(step*step), [&](int count)
        {
            const int j = step*(count / ((cols-sizepatch)/step));
            const int i = step*(count % ((cols-sizepatch)/step));

            //vec_t sample(sizepatch*sizepatch);

            if (i+sizepatch < cols && j+sizepatch < rows)
            for (unsigned int k=0;k<sizepatch*sizepatch;k++)
            //for_i(sizepatch*sizepatch, [&](int k)
            {
                unsigned int y = k / sizepatch + j;
                unsigned int x = k % sizepatch + i;
                res[count][k] = data[x+y*cols];
            }
            //);
            //res[count] = (sample);
        });


    return res;
}

template <typename Layer>
network<sequential>& operator << (network<sequential>& n, Layer&& l) {
    n.net_.add(std::make_shared<typename std::remove_reference<Layer>::type>(std::forward<Layer>(l)));
    return n;
}

template <typename NetType, typename Char, typename CharTraits>
std::basic_ostream<Char, CharTraits>& operator << (std::basic_ostream<Char, CharTraits>& os, const network<NetType>& n) {
    n.save(os);
    return os;
}

template <typename NetType, typename Char, typename CharTraits>
std::basic_istream<Char, CharTraits>& operator >> (std::basic_istream<Char, CharTraits>& os, network<NetType>& n) {
    n.load(os);
    return os;
}

} // namespace tiny_cnn
