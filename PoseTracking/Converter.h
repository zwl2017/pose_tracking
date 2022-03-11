#pragma once

#include <opencv2/core/core.hpp>

#include <Eigen/Dense>
#include "g2o/types/types_six_dof_expmap.h"
#include "g2o/types/types_seven_dof_expmap.h"

namespace PoseTracking 
{
	/**
	 * @brief 实现了 ORB-SLAM2中的一些常用的转换。
	 * @details 注意这是一个完全的静态类，没有成员变量，所有的成员函数均为静态的。
	 */
	class Converter
	{
	public:
		/**
		 * @brief 将以cv::Mat格式存储的位姿转换成为g2o::SE3Quat类型
		 *
		 * @param[in] 以cv::Mat格式存储的位姿
		 * @return g2o::SE3Quat 将以g2o::SE3Quat格式存储的位姿
		 */
		static g2o::SE3Quat toSE3Quat(const cv::Mat &cvT);

		/**
		 * @brief 将以g2o::SE3Quat格式存储的位姿转换成为cv::Mat格式
		 *
		 * @param[in] SE3 输入的g2o::SE3Quat格式存储的、待转换的位姿
		 * @return cv::Mat 转换结果
		 * @remark
		 */
		static cv::Mat toCvMat(const g2o::SE3Quat &SE3);

		/**
		 * @brief 将4x4 double型Eigen矩阵存储的位姿转换成为cv::Mat格式
		 *
		 * @paramp[in] m 输入Eigen矩阵
		 * @return cv::Mat 转换结果
		 * @remark
		 */
		static cv::Mat toCvMat(const Eigen::Matrix<double, 4, 4> &m);
	};
}